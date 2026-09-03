#include "helix/core.hpp"
#include "helix/common/error.hpp"
#include "helix/posix/abi_v3.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <new>
#include <thread>
#include <string_view>
#include <vector>
#ifdef __linux__
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/mempolicy.h>
#endif
namespace helix {
#ifdef __linux__
namespace {
constexpr auto kMagic = abi_v3::magic;
constexpr auto kAbi = abi_v3::version;
constexpr auto kCapacity = abi_v3::result_capacity;
constexpr auto kBatchResults = abi_v3::batch_results;
constexpr auto kUnderlyingCapacity = abi_v3::underlying_capacity;
using InitState = abi_v3::InitState;
using Header = abi_v3::Header;
using SharedMarket = abi_v3::MarketRegion;
using RingSlot = abi_v3::ResultBatch;
using SharedResults = abi_v3::ResultRegion;
std::uint64_t minimum_read(const SharedResults *results) noexcept {
  auto count = std::clamp<std::uint32_t>(results->consumer_count, 1,
                                         abi_v3::max_risk_consumers);
  auto minimum = results->reads[0].load(std::memory_order_acquire);
  for (std::uint32_t i = 1; i < count; ++i)
    minimum = std::min(minimum,
        results->reads[i].load(std::memory_order_acquire));
  return minimum;
}
bool apply_numa_policy(int node) noexcept {
  if (node < 0) return false;
#ifdef SYS_set_mempolicy
  constexpr std::size_t bits = sizeof(unsigned long) * 8;
  std::array<unsigned long, 1024 / bits> mask{};
  mask[static_cast<std::size_t>(node) / bits] |=
      1UL << (static_cast<unsigned>(node) % bits);
  return syscall(SYS_set_mempolicy, MPOL_BIND, mask.data(), 1024UL) == 0;
#else
  return false;
#endif
}
volatile std::sig_atomic_t signal_stop = 0;
void on_signal(int) noexcept { signal_stop = 1; }
enum class ShutdownReason : std::uint32_t { none, control, signal, dependency };
std::atomic<int> active_log_level{2};
void configure_logging(std::string_view level) noexcept {
  active_log_level.store(level == "trace" ? 0 : level == "debug" ? 1 :
                         level == "info" ? 2 : level == "warn" ? 3 : 4,
                         std::memory_order_relaxed);
}
bool info_logging() noexcept {
  return active_log_level.load(std::memory_order_relaxed) <= 2;
}
const char *shutdown_reason(std::uint32_t reason) noexcept {
  switch (static_cast<ShutdownReason>(reason)) {
  case ShutdownReason::control: return "control";
  case ShutdownReason::signal: return "signal";
  case ShutdownReason::dependency: return "dependency";
  default: return "none";
  }
}
void lifecycle_log(const char *service, const char *event,
                   std::string_view detail = {}) {
  if (!info_logging()) return;
  std::cout << "{\"timestamp_ns\":" << monotonic_ns() << ",\"service\":\""
            << service << "\",\"event\":\"" << event << "\"";
  if (!detail.empty()) std::cout << ",\"detail\":\"" << detail << "\"";
  std::cout << "}" << std::endl;
}
template <class T> class Mapping {
public:
  Mapping() = default;
  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;
  Mapping(Mapping &&o) noexcept
      : fd_(o.fd_), ptr_(o.ptr_), name_(std::move(o.name_)), owner_(o.owner_) {
    o.fd_ = -1;
    o.ptr_ = nullptr;
    o.owner_ = false;
  }
  ~Mapping() {
    if (ptr_)
      munmap(ptr_, sizeof(T));
    if (fd_ >= 0)
      close(fd_);
    if (owner_)
      shm_unlink(name_.c_str());
  }
  T *operator->() const { return ptr_; }
  T &operator*() const { return *ptr_; }
  T *get() const { return ptr_; }
  static Mapping open(const std::string &name, bool create, int retries = 0,
                      bool unlink_on_close = true) {
    Mapping m;
    m.name_ = name;
    m.owner_ = create && unlink_on_close;
    for (int i = 0;; ++i) {
      m.fd_ = shm_open(name.c_str(), O_RDWR | (create ? O_CREAT | O_EXCL : 0),
                       0600);
      if (m.fd_ >= 0)
        break;
      if (create && errno == EEXIST) {
        int existing = shm_open(name.c_str(), O_RDWR, 0);
        if (existing < 0)
          throw SystemError("shm_open_existing(" + name + ")");
        struct stat metadata {};
        if (fstat(existing, &metadata) != 0) {
          close(existing);
          throw SystemError("fstat(" + name + ")");
        }
        if (metadata.st_uid != geteuid()) {
          close(existing);
          throw Error(ErrorDomain::lifecycle, "create_shared_region",
                      "refusing to replace region owned by another user");
        }
        bool live = flock(existing, LOCK_EX | LOCK_NB) != 0 &&
                    (errno == EWOULDBLOCK || errno == EAGAIN);
        if (metadata.st_size >= static_cast<off_t>(sizeof(Header))) {
          void *header_memory = mmap(nullptr, sizeof(Header), PROT_READ,
                                     MAP_SHARED, existing, 0);
          if (header_memory != MAP_FAILED) {
            auto *header = static_cast<const Header *>(header_memory);
            if (header->magic == kMagic && header->creator_pid > 0) {
              errno = 0;
              live = kill(header->creator_pid, 0) == 0 || errno == EPERM;
            }
            munmap(header_memory, sizeof(Header));
          }
        }
        close(existing);
        if (live)
          throw Error(ErrorDomain::lifecycle, "create_shared_region",
                      "live creator already owns " + name);
        if (shm_unlink(name.c_str()) != 0)
          throw SystemError("shm_unlink_stale(" + name + ")");
        continue;
      }
      if (!create && errno == ENOENT && i < retries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      throw SystemError("shm_open(" + name + ")");
    }
    if (create && flock(m.fd_, LOCK_EX | LOCK_NB) != 0)
      throw SystemError("flock(" + name + ")");
    if (create && ftruncate(m.fd_, static_cast<off_t>(sizeof(T))) != 0)
      throw SystemError("ftruncate(" + name + ")");
    void *p =
        mmap(nullptr, sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED, m.fd_, 0);
    if (p == MAP_FAILED)
      throw SystemError("mmap(" + name + ")");
    m.ptr_ = static_cast<T *>(p);
    if (create)
      new (m.ptr_) T{};
    return m;
  }

private:
  int fd_{-1};
  T *ptr_{};
  std::string name_;
  bool owner_{};
};
void init_header(Header &h, std::size_t size, std::uint32_t capacity) {
  h.state.store(static_cast<std::uint32_t>(InitState::initializing));
  h.magic = kMagic;
  h.abi = kAbi;
  h.total_size = static_cast<std::uint32_t>(size);
  h.creator_pid = getpid();
  h.created_ns = monotonic_ns();
  h.capacity = capacity;
}
template <class T> bool header_valid(const T *p) {
  const auto &s = p->header;
  bool capacity_valid = true;
  if constexpr (std::is_same_v<T, SharedResults>)
    capacity_valid = s.capacity >= 2 && s.capacity <= kCapacity;
  return s.magic == kMagic && s.abi == kAbi && s.total_size == sizeof(T) &&
         capacity_valid &&
         s.state.load(std::memory_order_acquire) ==
             static_cast<std::uint32_t>(InitState::ready);
}
template <class T> void validate(T *p) {
  for (int retry = 0; retry < 500; ++retry) {
    if (header_valid(p))
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  throw Error(ErrorDomain::ipc, "validate_shared_region",
              "ABI, size, or initialization mismatch");
}
void initialize_market(SharedMarket *m) {
  init_header(m->header, sizeof(*m), 1);
  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
  int robust_result = pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
  if (robust_result != 0 && robust_result != ENOTSUP)
    throw std::runtime_error("pthread_mutexattr_setrobust failed");
  if (pthread_mutex_init(&m->mutex, &ma) != 0)
    throw std::runtime_error("pthread_mutex_init failed");
  pthread_mutexattr_destroy(&ma);
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
  if (pthread_cond_init(&m->changed, &ca) != 0)
    throw std::runtime_error("pthread_cond_init failed");
  pthread_condattr_destroy(&ca);
  m->snapshot = {};
  m->header.state.store(static_cast<std::uint32_t>(InitState::ready),
                        std::memory_order_release);
}
void lock_recover(SharedMarket *r) {
  int rc = pthread_mutex_lock(&r->mutex);
  if (rc == EOWNERDEAD) {
    r->snapshot = {};
    r->header.errors.fetch_add(1);
    pthread_mutex_consistent(&r->mutex);
    return;
  }
  if (rc)
    throw std::runtime_error("pthread_mutex_lock: " + std::to_string(rc));
}
bool lock_free_sequence(SharedMarket *m) {
  return decltype(m->sequence)::is_always_lock_free &&
         m->sequence.is_lock_free();
}
bool read_snapshot(SharedMarket *m, MarketSnapshot &out) {
  if (!lock_free_sequence(m)) {
    lock_recover(m);
    out = m->snapshot;
    pthread_mutex_unlock(&m->mutex);
    return true;
  }
  for (int retry = 0; retry < 1000; ++retry) {
    auto a = m->sequence.load(std::memory_order_acquire);
    if (a & 1U) {
      m->reader_retries.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    out = m->snapshot;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (a == m->sequence.load(std::memory_order_relaxed))
      return true;
    m->reader_retries.fetch_add(1, std::memory_order_relaxed);
  }
  m->header.errors.fetch_add(1);
  return false;
}
void publish(SharedMarket *m, const MarketSnapshot &s) {
  lock_recover(m);
  auto q = m->sequence.load();
  m->sequence.store(q + 1, std::memory_order_release);
  m->snapshot = s;
  m->sequence.store(q + 2, std::memory_order_release);
  m->header.generation.store(s.generation);
  m->snapshots_published.fetch_add(1, std::memory_order_relaxed);
  pthread_cond_broadcast(&m->changed);
  pthread_mutex_unlock(&m->mutex);
}
int make_server(const std::string &path) {
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
    throw std::runtime_error("unsafe Unix socket path");
  struct stat st {};
  if (lstat(path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid())
      throw std::runtime_error(
          "refusing to remove unowned or non-socket endpoint");
    if (unlink(path.c_str()))
      throw std::runtime_error("unlink stale socket failed");
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("socket failed");
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) || listen(fd, 8)) {
    close(fd);
    throw std::runtime_error("control socket bind/listen failed");
  }
  chmod(path.c_str(), 0600);
  return fd;
}
std::string receive_line(int fd) {
  std::string s;
  char b[128];
  while (s.size() < 4096) {
    auto n = read(fd, b, sizeof(b));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    s.append(b, static_cast<std::size_t>(n));
    auto p = s.find('\n');
    if (p != std::string::npos) {
      s.resize(p);
      break;
    }
  }
  if (s.size() >= 4096)
    return "__oversized__";
  return s;
}
std::string json_escape(const std::string &input) {
  std::string output;
  output.reserve(input.size());
  for (unsigned char c : input) {
    if (c == '"' || c == '\\') {
      output.push_back('\\');
      output.push_back(static_cast<char>(c));
    } else if (c == '\n')
      output += "\\n";
    else if (c == '\r')
      output += "\\r";
    else if (c == '\t')
      output += "\\t";
    else if (c >= 0x20)
      output.push_back(static_cast<char>(c));
  }
  return output;
}
void set_socket_timeouts(int fd) {
  timeval timeout{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
void send_all(int fd, const std::string &s) {
  std::size_t sent = 0;
  while (sent < s.size()) {
    auto n = write(fd, s.data() + sent, s.size() - sent);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    sent += static_cast<std::size_t>(n);
  }
}
std::string control_request(const Cli &c, const std::string &cmd) {
  if (c.socket_path.size() >= sizeof(sockaddr_un::sun_path))
    throw std::runtime_error("socket path too long");
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  set_socket_timeouts(fd);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, c.socket_path.c_str(),
              c.socket_path.size() + 1);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
    close(fd);
    throw std::runtime_error("connect: " + std::string(std::strerror(errno)));
  }
  send_all(fd, cmd + "\n");
  auto response = receive_line(fd);
  close(fd);
  return response;
}
std::string fragmented_control_request(const Cli &c) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) throw SystemError("socket(fragmented control)");
  set_socket_timeouts(fd);
  sockaddr_un address{}; address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, c.socket_path.c_str(), c.socket_path.size() + 1);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
    close(fd); throw SystemError("connect(fragmented control)");
  }
  send_all(fd, "sta");
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  send_all(fd, "tus\n");
  auto response = receive_line(fd); close(fd); return response;
}
std::string result_status_json(const std::string &name) {
  int fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (fd < 0)
    return "\"pricer\":{\"state\":\"unavailable\"},\"risk\":{\"state\":\"unavailable\"}";
  void *memory = mmap(nullptr, sizeof(SharedResults), PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (memory == MAP_FAILED)
    return "\"pricer\":{\"state\":\"unavailable\"},\"risk\":{\"state\":\"unavailable\"}";
  auto *r = static_cast<const SharedResults *>(memory);
  if (r->header.magic != kMagic || r->header.abi != kAbi ||
      r->header.total_size != sizeof(SharedResults)) {
    munmap(memory, sizeof(SharedResults));
    return "\"pricer\":{\"state\":\"abi_mismatch\"},\"risk\":{\"state\":\"unavailable\"}";
  }
  auto write = r->write.load(std::memory_order_acquire);
  auto read = minimum_read(r);
  auto now = monotonic_ns();
  auto pricer_heartbeat = r->pricer_heartbeat_ns.load();
  auto risk_heartbeat = r->risk_heartbeat_ns[0].load();
  auto pricer_age = pricer_heartbeat ? now - pricer_heartbeat : 0;
  auto risk_age = risk_heartbeat ? now - risk_heartbeat : 0;
  auto state = [&](std::int32_t pid, std::uint64_t heartbeat_age) {
    if (r->shutdown.load())
      return "stopped";
    if (pid == 0)
      return "starting";
    return heartbeat_age > 2'000'000'000ULL ? "stale" : "healthy";
  };
  auto percentile = [&](std::uint64_t numerator, std::uint64_t denominator) {
    auto total = r->latency_count.load(std::memory_order_acquire);
    if (!total)
      return std::uint64_t{0};
    auto target = (total * numerator + denominator - 1) / denominator;
    std::uint64_t cumulative = 0;
    for (std::size_t bucket = 0; bucket < 64; ++bucket) {
      cumulative += r->latency_histogram[bucket].load(std::memory_order_relaxed);
      if (cumulative >= target)
        return bucket >= 62 ? std::numeric_limits<std::uint64_t>::max()
                            : (std::uint64_t{1} << (bucket + 1));
    }
    return std::uint64_t{0};
  };
  std::string json =
      "\"pricer\":{\"state\":\"" +
      std::string(state(r->pricer_pid, pricer_age)) + "\",\"pid\":" +
      std::to_string(r->pricer_pid) + ",\"kernel\":\"" +
      std::string(r->active_kernel) + "\",\"worker_count\":" +
      std::to_string(r->worker_count.load()) + ",\"affinity_pins\":" +
      std::to_string(r->affinity_pins.load()) + ",\"realtime_workers\":" +
      std::to_string(r->realtime_workers.load()) +
      ",\"numa_node\":" + std::to_string(r->numa_node) +
      ",\"numa_applied\":" + (r->numa_applied.load() ? "true" : "false") +
      ",\"batches_published\":" +
      std::to_string(r->batches_published.load()) +
      ",\"options_priced\":" + std::to_string(r->options_priced.load()) +
      ",\"invalid_records\":" + std::to_string(r->invalid_records.load()) +
      ",\"iv_failures\":" + std::to_string(r->iv_failures.load()) +
      ",\"calibration_failures\":" +
      std::to_string(r->calibration_failures.load()) +
      ",\"worker_wakeups\":" + std::to_string(r->worker_wakeups.load()) +
      ",\"heartbeat_age_ns\":" +
      std::to_string(pricer_age) + "},\"risk\":{\"state\":\"" +
      std::string(state(r->risk_pids[0], risk_age)) + "\",\"pid\":" +
      std::to_string(r->risk_pids[0]) + ",\"consumer_count\":" +
      std::to_string(r->consumer_count) + ",\"batches_consumed\":" +
      std::to_string(r->batches_consumed.load()) +
      ",\"sequence_gaps\":" + std::to_string(r->sequence_gaps.load()) +
      ",\"heartbeat_age_ns\":" +
      std::to_string(risk_age) +
      ",\"latency_count\":" + std::to_string(r->latency_count.load()) +
      ",\"latency_min_ns\":" +
      std::to_string(r->latency_count.load() ? r->latency_min_ns.load() : 0) +
      ",\"latency_mean_ns\":" +
      std::to_string(r->latency_count.load()
                         ? r->latency_sum_ns.load() / r->latency_count.load()
                         : 0) +
      ",\"latency_max_ns\":" + std::to_string(r->latency_max_ns.load()) +
      ",\"latency_p50_ns\":" + std::to_string(percentile(50, 100)) +
      ",\"latency_p95_ns\":" + std::to_string(percentile(95, 100)) +
      ",\"latency_p99_ns\":" + std::to_string(percentile(99, 100)) +
      ",\"latency_p999_ns\":" + std::to_string(percentile(999, 1000)) +
      "},\"ring\":{\"depth\":" + std::to_string(write - read) +
      ",\"capacity\":" + std::to_string(r->header.capacity) +
      ",\"full_events\":" + std::to_string(r->ring_full_events.load()) +
      ",\"drops\":" + std::to_string(r->drops.load()) +
      ",\"high_water\":" + std::to_string(r->high_water.load()) +
      ",\"shutdown_reason\":\"" + shutdown_reason(r->shutdown_reason.load()) +
      "\"}";
  munmap(memory, sizeof(SharedResults));
  return json;
}
struct SpawnedProcess { pid_t pid{-1}; int output_fd{-1}; std::string label; };
std::string executable_directory() {
  char path[4096]{};
  auto length = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (length <= 0) throw SystemError("readlink(/proc/self/exe)");
  std::string full(path, static_cast<std::size_t>(length));
  auto slash = full.find_last_of('/');
  return slash == std::string::npos ? "." : full.substr(0, slash);
}
SpawnedProcess spawn_process(const std::string &path,
                             const std::vector<std::string> &arguments,
                             std::string label) {
  int descriptors[2];
  if (pipe2(descriptors, O_CLOEXEC) != 0) throw SystemError("pipe2");
  pid_t pid = fork();
  if (pid < 0) throw SystemError("fork");
  if (pid == 0) {
    close(descriptors[0]); dup2(descriptors[1], STDOUT_FILENO);
    dup2(descriptors[1], STDERR_FILENO); close(descriptors[1]);
    std::vector<char *> argv; argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char *>(path.c_str()));
    for (const auto &argument : arguments)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr); execv(path.c_str(), argv.data());
    dprintf(STDERR_FILENO, "execv(%s): %s\n", path.c_str(), std::strerror(errno));
    _exit(127);
  }
  close(descriptors[1]); return {pid, descriptors[0], std::move(label)};
}
std::jthread log_process(SpawnedProcess process) {
  return std::jthread([process = std::move(process)] {
    std::string pending; char buffer[256];
    for (;;) {
      auto count = read(process.output_fd, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      pending.append(buffer, static_cast<std::size_t>(count));
      std::size_t newline;
      while ((newline = pending.find('\n')) != std::string::npos) {
        std::cerr << '[' << process.label << "] " << pending.substr(0, newline)
                  << '\n'; pending.erase(0, newline + 1);
      }
    }
    if (!pending.empty()) std::cerr << '[' << process.label << "] " << pending << '\n';
    close(process.output_fd);
  });
}
} // namespace
int run_market_data(const Cli &c) {
  Cli live = c;
  configure_logging(c.log_level);
  signal_stop = 0;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  auto m = Mapping<SharedMarket>::open(c.market_name, true, 0,
      c.cleanup_policy == "owner");
  initialize_market(m.get());
  int sock = make_server(c.socket_path);
  bool paused = false;
  auto started = monotonic_ns();
  MarketSnapshot snap{};
  lifecycle_log("market_data", "ready",
                lock_free_sequence(m.get()) ? "lock-free" : "mutex");
  while (!signal_stop && !m->shutdown.load(std::memory_order_acquire)) {
    pollfd p{sock, POLLIN, 0};
    int ready = poll(&p, 1, std::max(1, 1000 / live.update_hz));
    if (ready < 0 && errno != EINTR)
      throw std::runtime_error("poll failed");
    if (ready > 0) {
      int cl = accept4(sock, nullptr, nullptr, SOCK_CLOEXEC);
      if (cl >= 0) {
        set_socket_timeouts(cl);
        auto cmd = receive_line(cl);
        std::string reply;
        if (cmd == "__oversized__") {
          reply = "{\"ok\":false,\"error\":\"command exceeds 4096 bytes\"}\n";
        } else if (cmd == "pause") {
          paused = true;
          reply = "{\"ok\":true,\"state\":\"paused\"}\n";
        } else if (cmd == "resume" || cmd == "start") {
          paused = false;
          reply = "{\"ok\":true,\"state\":\"running\"}\n";
        } else if (cmd == "reload") {
          if (live.config_path.empty()) {
            reply = "{\"ok\":false,\"error\":\"no configuration file supplied\"}\n";
          } else {
            try {
              char program[] = "helix_market_data", flag[] = "--config";
              std::vector<char> path(live.config_path.begin(), live.config_path.end());
              path.push_back('\0');
              char *args[]{program, flag, path.data()};
              auto next = parse_cli(3, args);
              if (next.market_name != live.market_name ||
                  next.result_name != live.result_name ||
                  next.socket_path != live.socket_path ||
                  next.ring_capacity != live.ring_capacity ||
                  next.underlyings != live.underlyings ||
                  next.universe != live.universe || next.workers != live.workers ||
                  next.kernel != live.kernel || next.sync != live.sync) {
                reply = "{\"ok\":false,\"error\":\"reload changes immutable topology or IPC settings\"}\n";
              } else {
                live.update_hz = next.update_hz;
                live.seed = next.seed;
                live.log_level = next.log_level;
                configure_logging(live.log_level);
                auto revision = m->config_revision.fetch_add(1) + 1;
                reply = "{\"ok\":true,\"reloaded\":true,\"update_hz\":" +
                        std::to_string(live.update_hz) +
                        ",\"config_revision\":" + std::to_string(revision) + "}\n";
              }
            } catch (const std::exception &e) {
              reply = "{\"ok\":false,\"error\":\"" +
                      json_escape(e.what()) + "\"}\n";
            }
          }
        }
        else if (cmd == "shutdown") {
          m->shutdown_reason.store(
              static_cast<std::uint32_t>(ShutdownReason::control));
          m->shutdown.store(true, std::memory_order_release);
          reply = "{\"ok\":true,\"state\":\"stopping\"}\n";
        } else if (cmd == "status")
          reply = "{\"ok\":true,\"market\":{\"state\":\"" +
                  std::string(paused ? "paused" : "running") +
                  "\",\"pid\":" + std::to_string(getpid()) +
                  ",\"uptime_ns\":" + std::to_string(monotonic_ns() - started) +
                  ",\"market_generation\":" + std::to_string(snap.generation) +
                  ",\"snapshots_published\":" +
                  std::to_string(m->snapshots_published.load()) +
                  ",\"snapshot_reader_retries\":" +
                  std::to_string(m->reader_retries.load()) +
                  ",\"invalid_records\":" +
                  std::to_string(m->invalid_records.load()) +
                  ",\"update_hz\":" + std::to_string(live.update_hz) +
                  ",\"config_revision\":" +
                  std::to_string(m->config_revision.load()) +
                  ",\"errors\":" + std::to_string(m->header.errors.load()) +
                  ",\"shutdown_reason\":\"" +
                  shutdown_reason(m->shutdown_reason.load()) +
                  "\",\"region_health\":\"ready\"}," +
                  result_status_json(live.result_name) + "}\n";
        else
          reply = "{\"ok\":false,\"error\":\"malformed or unknown command\"}\n";
        send_all(cl, reply);
        close(cl);
      }
    }
    if (!paused) {
      ++snap.generation;
      snap.timestamp_ns = monotonic_ns();
      snap.changed_underlying = static_cast<std::uint32_t>(
          snap.generation % live.underlyings);
      snap.spot = 100 + std::sin(static_cast<double>(snap.generation) * .01);
      snap.underlying_spots[snap.changed_underlying] = snap.spot;
      publish(m.get(), snap);
    }
  }
  if (signal_stop) {
    m->shutdown_reason.store(static_cast<std::uint32_t>(ShutdownReason::signal));
    m->shutdown.store(true, std::memory_order_release);
    pthread_cond_broadcast(&m->changed);
  }
  close(sock);
  unlink(c.socket_path.c_str());
  lifecycle_log("market_data", "stopped",
                shutdown_reason(m->shutdown_reason.load()));
  return 0;
}
int run_pricer(const Cli &c) {
  configure_logging(c.log_level);
  auto m = Mapping<SharedMarket>::open(c.market_name, false, 500);
  validate(m.get());
  auto r = Mapping<SharedResults>::open(c.result_name, true, 0,
      c.cleanup_policy == "owner");
  init_header(r->header, sizeof(*r), static_cast<std::uint32_t>(c.ring_capacity));
  r->latency_min_ns.store(std::numeric_limits<std::uint64_t>::max());
  r->pricer_pid = getpid();
  r->numa_node = c.numa_node;
  r->numa_applied.store(apply_numa_policy(c.numa_node));
  r->consumer_count = static_cast<std::uint32_t>(c.risk_consumers);
  std::strncpy(r->active_kernel, c.kernel.c_str(), sizeof(r->active_kernel) - 1);
  r->header.state.store(static_cast<std::uint32_t>(InitState::ready),
                        std::memory_order_release);
  OptionSoA u;
  u.reserve(c.universe);
  for (std::size_t i = 0; i < c.universe; ++i)
    u.push(i, 80.0 + static_cast<double>(i % 41),
           .05 + static_cast<double>(i % 20) * .05, .01,
           (i & 1) ? -1 : 1,
           (i & 1) ? OptionType::put : OptionType::call,
           static_cast<std::uint32_t>(i % c.underlyings));
  ResultBuffer out(c.universe);
  ResultBuffer affected;
  affected.reserve(c.universe);
  WorkerEngine engine(u, static_cast<std::size_t>(c.workers),
                      c.sync == "hybrid" ? SyncMode::hybrid_spin
                                         : SyncMode::condition_variable,
                      c.spin_ns, c.affinity, c.realtime_priority,
                      c.allowed_cpus);
  lifecycle_log("pricer", "ready", c.kernel);
  r->worker_count.store(static_cast<std::uint32_t>(engine.workers()));
  r->affinity_pins.store(static_cast<std::uint32_t>(engine.affinity_pins()));
  r->realtime_workers.store(
      static_cast<std::uint32_t>(engine.realtime_workers()));
  std::uint64_t seen = 0, batch = 0;
  while (!m->shutdown.load()) {
    MarketSnapshot s;
    if (read_snapshot(m.get(), s) && s.generation != seen) {
      if (c.kernel == "avx2-fast" || c.kernel == "avx512-fast") {
        auto vector_market = s;
        vector_market.changed_underlying =
            std::numeric_limits<std::uint32_t>::max();
        bool executed = c.kernel == "avx512-fast"
                            ? price_batch_avx512(u, vector_market, out)
                            : price_batch_avx2(u, vector_market, out);
        if (!executed) engine.price(s, out);
      }
      else if (c.kernel == "scalar-exact")
        for (std::size_t i = 0; i < u.size(); ++i) {
          if (u.underlying_id[i] != s.changed_underlying)
            continue;
          auto spot = s.underlying_spots[u.underlying_id[i] %
                                         s.underlying_spots.size()];
          out[i] = {u.id[i], u.underlying_id[i], u.quantity[i],
                    black_scholes({spot, u.strike[i], u.time[i], s.rate,
                                   u.dividend[i], s.volatility, u.type[i]},
                                  false)};
        }
      else if (c.kernel != "avx2-fast")
        engine.price(s, out);
      affected.clear();
      for (std::size_t i = 0; i < out.size(); ++i)
        if (u.underlying_id[i] == s.changed_underlying)
          affected.push_back(out[i]);
      for (std::size_t i = 0; i < affected.size(); i += kBatchResults) {
        auto w = r->write.load(), rd = minimum_read(r.get());
        while (w - rd >= r->header.capacity && c.drop_policy == "block" &&
               !m->shutdown.load(std::memory_order_acquire)) {
          r->ring_full_events.fetch_add(1, std::memory_order_relaxed);
          std::this_thread::sleep_for(std::chrono::microseconds(50));
          rd = minimum_read(r.get());
        }
        if (w - rd >= r->header.capacity) {
          r->ring_full_events.fetch_add(1, std::memory_order_relaxed);
          r->drops.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        auto count =
            std::min<std::size_t>(kBatchResults, affected.size() - i);
        auto &slot = r->slots[w % r->header.capacity];
        slot.market_generation = s.generation;
        slot.batch_id = ++batch;
        slot.first_instrument = i;
        slot.count = count;
        slot.market_publish_ns = s.timestamp_ns;
        slot.price_complete_ns = monotonic_ns();
        slot.validation_errors = 0;
        for (std::size_t j = 0; j < count; ++j)
          if (!std::isfinite(affected[i + j].greeks.price))
            ++slot.validation_errors;
        r->invalid_records.fetch_add(slot.validation_errors,
                                     std::memory_order_relaxed);
        std::copy_n(affected.data() + i, count, slot.results);
        r->write.store(w + 1, std::memory_order_release);
        r->batches_published.fetch_add(1, std::memory_order_relaxed);
        r->options_priced.fetch_add(count, std::memory_order_relaxed);
        auto depth = w + 1 - rd, old = r->high_water.load();
        while (depth > old &&
               !r->high_water.compare_exchange_weak(old, depth)) {
        }
      }
      r->pricer_heartbeat_ns.store(monotonic_ns(), std::memory_order_release);
      r->worker_wakeups.store(engine.wakeups(), std::memory_order_relaxed);
      seen = s.generation;
    } else
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  r->shutdown_reason.store(m->shutdown_reason.load(std::memory_order_acquire));
  r->shutdown.store(true, std::memory_order_release);
  lifecycle_log("pricer", "stopped",
                shutdown_reason(r->shutdown_reason.load()));
  return 0;
}
int run_risk(const Cli &c) {
  configure_logging(c.log_level);
  auto r = Mapping<SharedResults>::open(c.result_name, false, 500);
  validate(r.get());
  if (c.consumer_id < 0 ||
      static_cast<std::uint32_t>(c.consumer_id) >= r->consumer_count)
    throw Error(ErrorDomain::ipc, "attach_risk_consumer",
                "consumer id is outside the producer configuration");
  auto consumer = static_cast<std::size_t>(c.consumer_id);
  r->risk_pids[consumer] = getpid();
  lifecycle_log("risk", "ready");
  double d = 0, g = 0, v = 0, t = 0, rho = 0;
  std::array<double, kUnderlyingCapacity> underlying_delta{},
      underlying_gamma{}, underlying_vega{}, underlying_theta{},
      underlying_rho{};
  std::uint64_t count = 0, last = 0, gaps = 0;
  while (!r->shutdown.load() || r->reads[consumer].load() < r->write.load()) {
    auto rd = r->reads[consumer].load(), w = r->write.load(std::memory_order_acquire);
    if (rd == w) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    auto slot = r->slots[rd % r->header.capacity];
    auto consumed_ns = monotonic_ns();
    auto latency_ns = consumed_ns - slot.market_publish_ns;
    r->latency_count.fetch_add(1, std::memory_order_relaxed);
    r->latency_sum_ns.fetch_add(latency_ns, std::memory_order_relaxed);
    auto minimum = r->latency_min_ns.load(std::memory_order_relaxed);
    while (latency_ns < minimum &&
           !r->latency_min_ns.compare_exchange_weak(minimum, latency_ns)) {
    }
    auto maximum = r->latency_max_ns.load(std::memory_order_relaxed);
    while (latency_ns > maximum &&
           !r->latency_max_ns.compare_exchange_weak(maximum, latency_ns)) {
    }
    auto bucket = latency_ns ? static_cast<unsigned>(63 - __builtin_clzll(latency_ns))
                             : 0U;
    r->latency_histogram[std::min<unsigned>(bucket, 63)].fetch_add(
        1, std::memory_order_relaxed);
    if (last && slot.batch_id != last + 1)
      ++gaps;
    last = slot.batch_id;
    for (std::size_t i = 0; i < slot.count; ++i) {
      const auto &result = slot.results[i];
      auto underlying =
          std::min<std::size_t>(result.underlying_id, kUnderlyingCapacity - 1);
      auto quantity = result.quantity;
      auto delta = result.greeks.delta * quantity;
      auto gamma = result.greeks.gamma * quantity;
      auto vega = result.greeks.vega * quantity;
      auto theta = result.greeks.theta * quantity;
      auto result_rho = result.greeks.rho * quantity;
      d += delta;
      g += gamma;
      v += vega;
      t += theta;
      rho += result_rho;
      underlying_delta[underlying] += delta;
      underlying_gamma[underlying] += gamma;
      underlying_vega[underlying] += vega;
      underlying_theta[underlying] += theta;
      underlying_rho[underlying] += result_rho;
      ++count;
    }
    r->reads[consumer].store(rd + 1, std::memory_order_release);
    r->batches_consumed.fetch_add(1, std::memory_order_relaxed);
    r->sequence_gaps.store(gaps, std::memory_order_relaxed);
    r->risk_heartbeat_ns[consumer].store(monotonic_ns(), std::memory_order_release);
    // Keep long-running qualification logs useful without flooding stdout.
    if (info_logging() && count && count % (1U << 20U) == 0)
      std::cout << "{\"timestamp_ns\":" << monotonic_ns()
                << ",\"service\":\"risk\",\"event\":\"summary\""
                << ",\"count\":" << count << ",\"delta\":" << d
                << ",\"gamma\":" << g << ",\"vega\":" << v
                << ",\"theta\":" << t << ",\"rho\":" << rho
                << ",\"gaps\":" << gaps << ",\"underlying0_delta\":"
                << underlying_delta[0] << "}" << std::endl;
  }
  lifecycle_log("risk", "stopped",
                shutdown_reason(r->shutdown_reason.load()));
  return 0;
}
int run_ctl(const Cli &c, const std::string &cmd) {
  std::cout << control_request(c, cmd) << '\n';
  return 0;
}
int run_demo(const Cli &c) {
  std::string suffix = std::to_string(getpid());
  Cli x = c;
  x.market_name = "/helix_market_" + suffix;
  x.result_name = "/helix_results_" + suffix;
  x.socket_path = "/tmp/helix_" + suffix + ".sock";
  x.config_path = "/tmp/helix_" + suffix + ".toml";
  {
    std::ofstream config(x.config_path);
    config << "market_name = \"" << x.market_name << "\"\n"
           << "result_name = \"" << x.result_name << "\"\n"
           << "socket_path = \"" << x.socket_path << "\"\n"
           << "update_hz = 50\nworkers = " << x.workers
           << "\nuniverse_size = " << x.universe
           << "\nunderlyings = " << x.underlyings
           << "\nring_capacity = " << x.ring_capacity
           << "\nrisk_consumers = " << x.risk_consumers
           << "\nconsumer_id = 0"
           << "\nnuma_node = " << x.numa_node
           << "\ndrop_policy = \"" << x.drop_policy << "\""
           << "\nsync = \"" << x.sync << "\""
           << "\nspin_ns = " << x.spin_ns
           << "\naffinity = " << (x.affinity ? "true" : "false")
           << "\nrealtime_priority = " << x.realtime_priority
           << "\nallowed_cpus = \"" << x.allowed_cpus << "\""
           << "\ncleanup_policy = \"" << x.cleanup_policy << "\""
           << "\nlog_level = \"" << x.log_level << "\""
           << "\nseed = " << x.seed
           << "\nkernel = \"" << x.kernel << "\"\n";
  }
  auto directory = executable_directory();
  std::vector<pid_t> children;
  std::vector<std::jthread> loggers;
  auto launch = [&](const std::string &binary, std::vector<std::string> args,
                    const std::string &label) {
    auto process = spawn_process(directory + "/" + binary, args, label);
    children.push_back(process.pid); loggers.push_back(log_process(std::move(process)));
  };
  auto endpoints = std::vector<std::string>{"--config", x.config_path,
      "--market", x.market_name, "--results", x.result_name,
      "--socket", x.socket_path};
  launch("helix_market_data", endpoints, "market");
  bool market_ready = false;
  for (int attempt = 0; attempt < 60 && !market_ready; ++attempt) {
    try { market_ready = control_request(x, "status").find("\"ok\":true") != std::string::npos; }
    catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
  }
  if (!market_ready) {
    for (auto pid : children) kill(pid, SIGTERM);
    for (auto pid : children) waitpid(pid, nullptr, 0);
    unlink(x.config_path.c_str()); std::cerr << "demo readiness timeout: market\n"; return 1;
  }
  auto pricer_args = endpoints;
  pricer_args.insert(pricer_args.end(), {"--workers", std::to_string(x.workers),
      "--kernel", x.kernel, "--sync", x.sync, "--spin-ns",
      std::to_string(x.spin_ns)});
  launch("helix_pricer", pricer_args, "pricer");
  for (int consumer = 0; consumer < x.risk_consumers; ++consumer) {
    auto risk_args = endpoints;
    risk_args.insert(risk_args.end(), {"--risk-consumers",
        std::to_string(x.risk_consumers), "--consumer-id",
        std::to_string(consumer)});
    launch("helix_risk", risk_args, "risk" + std::to_string(consumer));
  }
  bool services_ready = false;
  for (int attempt = 0; attempt < 100 && !services_ready; ++attempt) {
    try { auto status = control_request(x, "status");
      services_ready = status.find("\"pricer\":{\"state\":\"healthy\"") != std::string::npos &&
                       status.find("\"risk\":{\"state\":\"healthy\"") != std::string::npos; }
    catch (...) {}
    if (!services_ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!services_ready) {
    try { (void)control_request(x, "shutdown"); } catch (...) {}
    for (auto pid : children) kill(pid, SIGTERM);
    for (auto pid : children) waitpid(pid, nullptr, 0);
    unlink(x.config_path.c_str()); std::cerr << "demo readiness timeout: services\n"; return 1;
  }
  int ctl = 0;
  for (int tick = 0; tick < std::max(1, x.duration) * 10; ++tick) {
    bool early_exit = false;
    for (auto pid : children) { siginfo_t info{}; if (waitid(P_PID, pid, &info, WEXITED|WNOHANG|WNOWAIT)==0 && info.si_pid!=0) early_exit=true; }
    if (early_exit) { ctl = 1; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  auto status_reply = control_request(x, "status");
  std::cout << status_reply << '\n';
  ctl |= status_reply.find("\"kernel\":\"" + x.kernel + "\"") ==
                    std::string::npos ||
                status_reply.find("\"drops\":0") == std::string::npos ||
                status_reply.find("\"sequence_gaps\":0") == std::string::npos ||
                status_reply.find("\"latency_count\":0") != std::string::npos
             ? 1 : 0;
  if (status_reply.find("\"consumer_count\":" +
                        std::to_string(x.risk_consumers)) == std::string::npos)
    ctl = 1;
  ctl |= run_ctl(x, "pause");
  auto reload_reply = control_request(x, "reload");
  std::cout << reload_reply << '\n';
  if (reload_reply.find("\"update_hz\":50") == std::string::npos)
    ctl = 1;
  {
    std::ofstream invalid_config(x.config_path, std::ios::trunc);
    invalid_config << "market_name = \"" << x.market_name << "\"\n"
                   << "result_name = \"" << x.result_name << "\"\n"
                   << "socket_path = \"/tmp/forbidden_reload.sock\"\n";
  }
  auto rejected_reload = control_request(x, "reload");
  std::cout << rejected_reload << '\n';
  if (rejected_reload.find("immutable topology or IPC settings") ==
      std::string::npos)
    ctl = 1;
  ctl |= run_ctl(x, "resume");
  std::atomic<int> client_failures{0};
  std::array<std::jthread, 4> clients;
  for (auto &client : clients)
    client = std::jthread([&] {
      try {
        if (control_request(x, "status").find("\"ok\":true") ==
            std::string::npos)
          client_failures.fetch_add(1);
      } catch (...) {
        client_failures.fetch_add(1);
      }
    });
  for (auto &client : clients)
    client.join();
  if (client_failures.load() != 0)
    ctl = 1;
  auto fragmented = fragmented_control_request(x);
  std::cout << fragmented << '\n';
  if (fragmented.find("\"ok\":true") == std::string::npos)
    ctl = 1;
  auto oversized = control_request(x, std::string(5000, 'x'));
  std::cout << oversized << '\n';
  if (oversized.find("exceeds 4096 bytes") == std::string::npos)
    ctl = 1;
  ctl |= run_ctl(x, "malformed");
  ctl |= run_ctl(x, "shutdown");
  int status = 0;
  for (pid_t p : children) {
    int child = 0;
    if (waitpid(p, &child, 0) < 0 || !WIFEXITED(child) ||
        WEXITSTATUS(child) != 0)
      status = 1;
  }
  unlink(x.config_path.c_str());
  std::cout << (status || ctl ? "demo failed" : "demo success") << '\n';
  return status || ctl;
}
bool run_ipc_failure_self_test() {
  auto name = "/helix_failure_" + std::to_string(getpid());
  auto mapping = Mapping<SharedMarket>::open(name, true);
  initialize_market(mapping.get());
  bool rejects_live_owner = false;
  try { (void)Mapping<SharedMarket>::open(name, true); }
  catch (const Error &) { rejects_live_owner = true; }
  pid_t waiter = fork();
  if (waiter == 0) {
    if (pthread_mutex_lock(&mapping->mutex) != 0) _exit(3);
    while (mapping->snapshot.generation == 0)
      if (pthread_cond_wait(&mapping->changed, &mapping->mutex) != 0) _exit(4);
    bool valid_snapshot = mapping->snapshot.generation == 77;
    pthread_mutex_unlock(&mapping->mutex);
    _exit(valid_snapshot ? 0 : 5);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  MarketSnapshot condition_snapshot{}; condition_snapshot.generation = 77;
  publish(mapping.get(), condition_snapshot);
  int waiter_status{};
  bool condition_transfer = waitpid(waiter, &waiter_status, 0) == waiter &&
      WIFEXITED(waiter_status) && WEXITSTATUS(waiter_status) == 0;
  mapping->snapshot = {};
  pid_t child = fork();
  if (child == 0) {
    if (pthread_mutex_lock(&mapping->mutex) != 0)
      _exit(2);
    mapping->snapshot.generation = 999;
    _exit(0);
  }
  int status = 0;
  if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0)
    return false;
  lock_recover(mapping.get());
  bool recovered = mapping->header.errors.load() == 1 &&
                   mapping->snapshot.generation == 0;
  pthread_mutex_unlock(&mapping->mutex);
  auto original_abi = mapping->header.abi;
  mapping->header.abi = original_abi + 1;
  bool rejects_abi = !header_valid(mapping.get());
  mapping->header.abi = original_abi;
  auto original_size = mapping->header.total_size;
  mapping->header.total_size = original_size - 1;
  bool rejects_size = !header_valid(mapping.get());
  mapping->header.total_size = original_size;
  mapping->header.state.store(static_cast<std::uint32_t>(InitState::initializing));
  bool rejects_partial = !header_valid(mapping.get());
  return rejects_live_owner && condition_transfer && recovered && rejects_abi &&
         rejects_size && rejects_partial;
}
#else
namespace {
int unavailable() {
  std::cerr << "Helix POSIX runtime requires Linux\n";
  return 2;
}
} // namespace
int run_market_data(const Cli &) { return unavailable(); }
int run_pricer(const Cli &) { return unavailable(); }
int run_risk(const Cli &) { return unavailable(); }
int run_ctl(const Cli &, const std::string &) { return unavailable(); }
int run_demo(const Cli &) { return unavailable(); }
bool run_ipc_failure_self_test() { return false; }
#endif
} // namespace helix
