#include "helix/core.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <new>
#include <numeric>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <mqueue.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <pthread.h>
#include <unistd.h>
#endif
namespace {
void report(const char *n, std::vector<double> v) {
  std::sort(v.begin(), v.end());
  auto q = [&](double p) {
    return v[static_cast<std::size_t>(p * (v.size() - 1))];
  };
  std::cout << "{\"benchmark\":\"" << n << "\",\"samples\":" << v.size()
            << ",\"p50_ns\":" << q(.5) << ",\"p95_ns\":" << q(.95)
            << ",\"p99_ns\":" << q(.99) << ",\"p999_ns\":" << q(.999)
            << ",\"round_trips_per_second\":"
            << 1e9 * v.size() / std::accumulate(v.begin(), v.end(), 0.0)
            << "}\n";
}
} // namespace
int main() {
#ifdef __linux__
  constexpr int samples = 10000;
  std::vector<double> v;
  v.reserve(samples);
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0) {
    char x = 'x', y{};
    auto child = fork();
    if (child == 0) {
      close(sockets[0]);
      while (read(sockets[1], &y, 1) == 1)
        if (write(sockets[1], &y, 1) != 1) break;
      _exit(0);
    }
    close(sockets[1]);
    for (int i = 0; i < samples; ++i) {
      auto a = helix::monotonic_ns();
      write(sockets[0], &x, 1);
      read(sockets[0], &y, 1);
      v.push_back(static_cast<double>(helix::monotonic_ns() - a));
    }
    report("unix_socket_cross_process_rtt", v);
    close(sockets[0]);
    waitpid(child, nullptr, 0);
  }
  std::string request_name = "/helix_mq_req_" + std::to_string(getpid());
  std::string response_name = "/helix_mq_rsp_" + std::to_string(getpid());
  mq_attr attr{};
  attr.mq_maxmsg = 10;
  attr.mq_msgsize = 8;
  mqd_t request = mq_open(request_name.c_str(), O_CREAT | O_RDWR, 0600, &attr);
  mqd_t response = mq_open(response_name.c_str(), O_CREAT | O_RDWR, 0600, &attr);
  if (request == static_cast<mqd_t>(-1) || response == static_cast<mqd_t>(-1)) {
    if (request != static_cast<mqd_t>(-1)) mq_close(request);
    if (response != static_cast<mqd_t>(-1)) mq_close(response);
    mq_unlink(request_name.c_str()); mq_unlink(response_name.c_str());
    attr.mq_maxmsg = 4;
    request = mq_open(request_name.c_str(), O_CREAT | O_RDWR, 0600, &attr);
    response = mq_open(response_name.c_str(), O_CREAT | O_RDWR, 0600, &attr);
  }
  if (request != static_cast<mqd_t>(-1) && response != static_cast<mqd_t>(-1)) {
    std::uint64_t x = 1, y = 0;
    auto child = fork();
    if (child == 0) {
      for (int i = 0; i < samples; ++i) {
        if (mq_receive(request, reinterpret_cast<char *>(&y), sizeof(y), nullptr) < 0 ||
            mq_send(response, reinterpret_cast<char *>(&y), sizeof(y), 0) < 0)
          _exit(2);
      }
      _exit(0);
    }
    v.clear();
    for (int i = 0; i < samples; ++i) {
      auto a = helix::monotonic_ns();
      mq_send(request, reinterpret_cast<char *>(&x), sizeof(x), 0);
      mq_receive(response, reinterpret_cast<char *>(&y), sizeof(y), nullptr);
      v.push_back(static_cast<double>(helix::monotonic_ns() - a));
    }
    report("posix_message_queue_cross_process_rtt", v);
    waitpid(child, nullptr, 0);
    mq_close(request); mq_close(response);
    mq_unlink(request_name.c_str()); mq_unlink(response_name.c_str());
  } else
    std::cout
        << "{\"benchmark\":\"posix_message_queue_cross_process_rtt\",\"skipped\":true}\n";
  struct SharedPing {
    alignas(64) std::atomic<std::uint64_t> request{};
    alignas(64) std::atomic<std::uint64_t> response{};
  };
  auto *ping = static_cast<SharedPing *>(mmap(nullptr, sizeof(SharedPing),
      PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (ping != MAP_FAILED) {
    new (ping) SharedPing{};
    auto child = fork();
    if (child == 0) {
      for (std::uint64_t expected = 1; expected <= samples; ++expected) {
        while (ping->request.load(std::memory_order_acquire) != expected) {}
        ping->response.store(expected, std::memory_order_release);
      }
      _exit(0);
    }
    v.clear();
    for (std::uint64_t i = 1; i <= samples; ++i) {
      auto a = helix::monotonic_ns();
      ping->request.store(i, std::memory_order_release);
      while (ping->response.load(std::memory_order_acquire) != i) {}
      v.push_back(static_cast<double>(helix::monotonic_ns() - a));
    }
    report("shared_memory_sequence_poll_cross_process_rtt", v);
    waitpid(child, nullptr, 0);
    munmap(ping, sizeof(SharedPing));
  }
  struct SharedCondition {
    pthread_mutex_t mutex; pthread_cond_t changed;
    std::uint64_t request{}, response{};
  };
  auto *condition = static_cast<SharedCondition *>(mmap(nullptr, sizeof(SharedCondition),
      PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (condition != MAP_FAILED) {
    new (condition) SharedCondition{};
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&condition->mutex, &ma); pthread_mutexattr_destroy(&ma);
    pthread_condattr_t ca; pthread_condattr_init(&ca);
    pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&condition->changed, &ca); pthread_condattr_destroy(&ca);
    auto child = fork();
    if (child == 0) {
      for (std::uint64_t expected = 1; expected <= samples; ++expected) {
        pthread_mutex_lock(&condition->mutex);
        while (condition->request != expected)
          pthread_cond_wait(&condition->changed, &condition->mutex);
        condition->response = expected;
        pthread_cond_broadcast(&condition->changed);
        pthread_mutex_unlock(&condition->mutex);
      }
      _exit(0);
    }
    v.clear();
    for (std::uint64_t i = 1; i <= samples; ++i) {
      auto a = helix::monotonic_ns();
      pthread_mutex_lock(&condition->mutex); condition->request = i;
      pthread_cond_broadcast(&condition->changed);
      while (condition->response != i)
        pthread_cond_wait(&condition->changed, &condition->mutex);
      pthread_mutex_unlock(&condition->mutex);
      v.push_back(static_cast<double>(helix::monotonic_ns() - a));
    }
    report("shared_memory_condition_cross_process_rtt", v);
    waitpid(child, nullptr, 0);
    pthread_cond_destroy(&condition->changed); pthread_mutex_destroy(&condition->mutex);
    munmap(condition, sizeof(SharedCondition));
  }
#else
  std::cout << "{\"benchmark\":\"ipc\",\"skipped\":true,\"reason\":\"Linux "
               "required\"}\n";
#endif
}
