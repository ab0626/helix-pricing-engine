#include "helix/core.hpp"
#include <algorithm>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace helix {
struct WorkerEngine::Impl {
  const OptionSoA &u;
  std::size_t count;
  SyncMode mode;
  std::uint64_t spin_ns;
  bool affinity;
  int realtime_priority;
  std::vector<int> requested_cpus;
  std::vector<std::jthread> threads;
  std::mutex mutex;
  std::condition_variable cv_job, cv_done;
  MarketSnapshot market{};
  std::span<Result> output{};
  std::atomic<std::uint64_t> generation{};
  std::size_t completed{};
  bool stopping{};
  alignas(64) std::atomic<std::uint64_t> wakeup_count{};
  std::atomic<std::size_t> pinned{}, realtime{};
  Impl(const OptionSoA &x, std::size_t n, SyncMode m, std::uint64_t spin,
       bool pin, int rt, const std::string &cpu_list)
      : u(x), count(std::max<std::size_t>(1, std::min<std::size_t>(8, n))),
        mode(m), spin_ns(spin), affinity(pin), realtime_priority(rt) {
    std::istringstream cpus(cpu_list);
    for (std::string item; std::getline(cpus, item, ',');)
      if (!item.empty()) requested_cpus.push_back(std::stoi(item));
    threads.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
      threads.emplace_back([this, i](std::stop_token st) { run(i, st); });
  }
  void run(std::size_t index, std::stop_token st) {
#ifdef __linux__
    if (affinity) {
      cpu_set_t allowed;
      CPU_ZERO(&allowed);
      if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) ==
          0) {
        std::vector<int> cpus;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
          if (CPU_ISSET(cpu, &allowed) &&
              (requested_cpus.empty() ||
               std::find(requested_cpus.begin(), requested_cpus.end(), cpu) !=
                   requested_cpus.end()))
            cpus.push_back(cpu);
        if (!cpus.empty()) {
          cpu_set_t target;
          CPU_ZERO(&target);
          CPU_SET(cpus[index % cpus.size()], &target);
          if (pthread_setaffinity_np(pthread_self(), sizeof(target), &target) ==
              0)
            pinned.fetch_add(1);
        }
      }
    }
    if (realtime_priority > 0) {
      sched_param p{};
      p.sched_priority = realtime_priority;
      if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0)
        realtime.fetch_add(1);
    }
#endif
    std::uint64_t seen = 0;
    while (!st.stop_requested()) {
      MarketSnapshot snap;
      std::span<Result> out;
      {
        std::unique_lock lock(mutex);
        if (mode == SyncMode::hybrid_spin) {
          lock.unlock();
          auto begin = monotonic_ns();
          while (generation.load(std::memory_order_acquire) == seen &&
                 !st.stop_requested() && monotonic_ns() - begin < spin_ns)
            std::this_thread::yield();
          lock.lock();
        }
        cv_job.wait(lock, [&] {
          return stopping ||
                 generation.load(std::memory_order_relaxed) != seen ||
                 st.stop_requested();
        });
        if (stopping || st.stop_requested())
          return;
        seen = generation.load(std::memory_order_relaxed);
        snap = market;
        out = output;
      }
      wakeup_count.fetch_add(1, std::memory_order_relaxed);
      auto first = u.size() * index / count,
           last = u.size() * (index + 1) / count;
      for (std::size_t i = first; i < last && i < out.size(); ++i) {
        if (snap.changed_underlying !=
                std::numeric_limits<std::uint32_t>::max() &&
            u.underlying_id[i] != snap.changed_underlying)
          continue;
        auto spot = snap.underlying_spots[u.underlying_id[i] %
                                             snap.underlying_spots.size()];
        out[i] = {u.id[i], u.underlying_id[i], u.quantity[i],
                  black_scholes({spot, u.strike[i], u.time[i], snap.rate,
                                 u.dividend[i], snap.volatility, u.type[i]},
                                true)};
      }
      {
        std::lock_guard lock(mutex);
        if (++completed == count)
          cv_done.notify_one();
      }
    }
  }
  ~Impl() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    cv_job.notify_all();
    for (auto &t : threads)
      t.request_stop();
  }
};
WorkerEngine::WorkerEngine(const OptionSoA &u, std::size_t n, SyncMode m,
                           std::uint64_t s, bool a, int rt, std::string cpus)
    : impl_(std::make_unique<Impl>(u, n, m, s, a, rt, cpus)) {}
WorkerEngine::~WorkerEngine() = default;
void WorkerEngine::price(const MarketSnapshot &m, std::span<Result> out) {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->market = m;
    impl_->output = out;
    impl_->completed = 0;
    impl_->generation.fetch_add(1, std::memory_order_release);
  }
  impl_->cv_job.notify_all();
  std::unique_lock lock(impl_->mutex);
  impl_->cv_done.wait(lock, [&] { return impl_->completed == impl_->count; });
}
std::size_t WorkerEngine::workers() const noexcept { return impl_->count; }
std::uint64_t WorkerEngine::wakeups() const noexcept {
  return impl_->wakeup_count.load(std::memory_order_relaxed);
}
std::size_t WorkerEngine::affinity_pins() const noexcept {
  return impl_->pinned.load(std::memory_order_relaxed);
}
std::size_t WorkerEngine::realtime_workers() const noexcept {
  return impl_->realtime.load(std::memory_order_relaxed);
}
} // namespace helix
