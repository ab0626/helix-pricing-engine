#include "helix/core.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sys/utsname.h>
#endif
namespace {
std::atomic<double> benchmark_sink{};
struct Stats {
  double min, max, mean, p50, p95, p99, p999;
};
Stats stats(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  auto q = [&](double p) {
    return v[static_cast<std::size_t>(p * (v.size() - 1))];
  };
  return {
      v.front(), v.back(), std::accumulate(v.begin(), v.end(), 0.0) / v.size(),
      q(.5),     q(.95),   q(.99),
      q(.999)};
}
void print(const std::string &n, const Stats &s) {
  std::cout << "{\"benchmark\":\"" << n
            << "\",\"count\":100,\"min_ns\":" << s.min
            << ",\"max_ns\":" << s.max << ",\"mean_ns\":" << s.mean
            << ",\"p50_ns\":" << s.p50 << ",\"p95_ns\":" << s.p95
            << ",\"p99_ns\":" << s.p99 << ",\"p999_ns\":" << s.p999 << "}\n";
}
helix::OptionSoA make_universe(std::size_t n) {
  helix::OptionSoA u;
  u.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
    u.push(i, 80 + i % 41, .05 + (i % 20) * .05, .01, 1,
           (i & 1) ? helix::OptionType::put : helix::OptionType::call);
  return u;
}
struct IvContext { helix::Input input; double price; };
double iv_residual(double volatility, void *raw) noexcept {
  auto &context = *static_cast<IvContext *>(raw);
  context.input.volatility = volatility;
  return helix::black_scholes(context.input).price - context.price;
}
} // namespace
int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "all";
  std::size_t n = argc > 2 ? std::stoull(argv[2]) : 20000;
  auto u = make_universe(n);
  std::vector<helix::Result> r(n);
  helix::MarketSnapshot m;
#ifdef __linux__
  utsname system{}; uname(&system);
  std::cout << "{\"benchmark_environment\":true,\"system\":\"" << system.sysname
            << "\",\"release\":\"" << system.release << "\",\"machine\":\""
            << system.machine << "\",\"compiler\":\"" << __VERSION__
            << "\",\"logical_cpus\":" << std::thread::hardware_concurrency()
#ifdef NDEBUG
            << ",\"build\":\"Release\"}\n";
#else
            << ",\"build\":\"Debug\"}\n";
#endif
#endif
  auto kernel = [&] {
    std::vector<double> v;
    std::vector<helix::Input> aos;
    std::vector<helix::Greeks> aos_out(n);
    aos.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
      aos.push_back({m.spot, u.strike[i], u.time[i], m.rate, u.dividend[i],
                     m.volatility, u.type[i]});
    for (int k = 0; k < 100; ++k) {
      auto a = helix::monotonic_ns();
      for (std::size_t i = 0; i < n; ++i)
        aos_out[i] = helix::black_scholes(aos[i], false);
      v.push_back(static_cast<double>(helix::monotonic_ns() - a) / n);
    }
    print("scalar_aos_exact_ns_per_option", stats(v));
    benchmark_sink.store(aos_out[n / 2].price, std::memory_order_relaxed);
    v.clear();
    helix::price_batch(u, m, r, false);
    for (int k = 0; k < 100; ++k) {
      auto a = helix::monotonic_ns();
      helix::price_batch(u, m, r, false);
      v.push_back(static_cast<double>(helix::monotonic_ns() - a) / n);
    }
    print("scalar_soa_exact_ns_per_option", stats(v));
    benchmark_sink.store(r[n / 2].greeks.price, std::memory_order_relaxed);
    v.clear();
    helix::price_batch(u, m, r);
    for (int k = 0; k < 100; ++k) {
      auto a = helix::monotonic_ns();
      helix::price_batch(u, m, r);
      v.push_back(static_cast<double>(helix::monotonic_ns() - a) / n);
    }
    print("scalar_soa_fast_ns_per_option", stats(v));
    benchmark_sink.store(r[n / 2].greeks.price, std::memory_order_relaxed);
#ifdef HELIX_HAS_AVX2_KERNEL
    if (helix::avx2_available()) {
      v.clear();
      (void)helix::price_batch_avx2(u, m, r);
      for (int k = 0; k < 100; ++k) {
        auto a = helix::monotonic_ns();
        (void)helix::price_batch_avx2(u, m, r);
        v.push_back(static_cast<double>(helix::monotonic_ns() - a) / n);
      }
      print("avx2_fast_ns_per_option", stats(v));
      benchmark_sink.store(r[n / 2].greeks.price, std::memory_order_relaxed);
    }
#endif
#ifdef HELIX_HAS_AVX512_KERNEL
    if (helix::avx512_available()) {
      v.clear(); (void)helix::price_batch_avx512(u, m, r);
      for (int k = 0; k < 100; ++k) {
        auto a = helix::monotonic_ns();
        (void)helix::price_batch_avx512(u, m, r);
        v.push_back(static_cast<double>(helix::monotonic_ns() - a) / n);
      }
      print("avx512_fast_ns_per_option", stats(v));
      benchmark_sink.store(r[n / 2].greeks.price, std::memory_order_relaxed);
    }
#endif
  };
  auto scaling = [&] {
    double baseline = 0;
    for (std::size_t workers : {1U, 2U, 4U, 8U}) {
      helix::WorkerEngine e(u, workers, helix::SyncMode::condition_variable, 0,
                            false);
      std::vector<double> v;
      e.price(m, r);
      for (int k = 0; k < 100; ++k) {
        auto a = helix::monotonic_ns();
        e.price(m, r);
        v.push_back(static_cast<double>(helix::monotonic_ns() - a));
      }
      auto measured = stats(v);
      print("workers_" + std::to_string(workers), measured);
      if (workers == 1) baseline = measured.p50;
      auto speedup = baseline / measured.p50;
      std::cout << "{\"benchmark\":\"worker_scaling\",\"workers\":" << workers
                << ",\"options_per_second\":"
                << (1e9 * static_cast<double>(n) / measured.p50)
                << ",\"speedup\":" << speedup << ",\"efficiency\":"
                << speedup / static_cast<double>(workers) << "}\n";
    }
  };
  auto cdf = [&] {
    double maxe = 0, rms = 0;
    auto a = helix::monotonic_ns();
    for (int repeat = 0; repeat < 100; ++repeat)
      for (int i = -8000; i <= 8000; ++i) {
        double x = i / 1000.0,
               e = helix::fast_normal_cdf(x) - helix::normal_cdf(x);
        maxe = std::max(maxe, std::abs(e));
        rms += e * e;
      }
    std::cout << "{\"benchmark\":\"cdf_accuracy\",\"max_abs_error\":" << maxe
              << ",\"rms_error\":" << std::sqrt(rms / (100.0 * 16001))
              << ",\"elapsed_ns\":" << helix::monotonic_ns() - a << "}\n";
  };
  auto iv = [&] {
    std::vector<double> nw, bi, bt;
    std::uint64_t ni = 0, bii = 0, bti = 0, nf = 0, bif = 0, btf = 0;
    double nrmax = 0, birmax = 0, btrmax = 0;
    for (int repeat = 0; repeat < 20; ++repeat)
      for (double vol : {.05, .1, .2, .4, .8}) {
        helix::Input x{100,
                       static_cast<double>(90 + repeat % 5 * 5),
                       1,
                       .03,
                       .01,
                       vol,
                       helix::OptionType::call};
        double price = helix::black_scholes(x).price;
        auto a = helix::monotonic_ns();
        auto nr = helix::implied_volatility(x, price);
        nw.push_back(static_cast<double>(helix::monotonic_ns() - a));
        a = helix::monotonic_ns();
        auto br = helix::implied_volatility_bisection(x, price);
        bi.push_back(static_cast<double>(helix::monotonic_ns() - a));
        IvContext context{x, price};
        a = helix::monotonic_ns();
        auto btr = helix::brent(iv_residual, &context, 1e-8, 5.0, 1e-10);
        bt.push_back(static_cast<double>(helix::monotonic_ns() - a));
        ni += static_cast<std::uint64_t>(nr.iterations);
        bii += static_cast<std::uint64_t>(br.iterations);
        bti += static_cast<std::uint64_t>(btr.iterations);
        nf += nr.status != helix::SolveStatus::converged;
        bif += br.status != helix::SolveStatus::converged;
        btf += btr.status != helix::SolveStatus::converged;
        nrmax = std::max(nrmax, std::abs(nr.residual));
        birmax = std::max(birmax, std::abs(br.residual));
        btrmax = std::max(btrmax, std::abs(btr.residual));
      }
    print("iv_safeguarded_newton", stats(nw));
    print("iv_bisection", stats(bi));
    print("iv_brent", stats(bt));
    std::cout << "{\"benchmark\":\"iv_iterations\",\"newton_mean\":"
              << ni / 100.0 << ",\"bisection_mean\":" << bii / 100.0
              << ",\"brent_mean\":" << bti / 100.0
              << ",\"newton_failures\":" << nf
              << ",\"bisection_failures\":" << bif
              << ",\"brent_failures\":" << btf
              << ",\"newton_max_residual\":" << nrmax
              << ",\"bisection_max_residual\":" << birmax
              << ",\"brent_max_residual\":" << btrmax << "}\n";
  };
  auto sync = [&] {
    for (auto sm :
         {helix::SyncMode::condition_variable, helix::SyncMode::hybrid_spin}) {
      helix::WorkerEngine e(u, 2, sm, 50000, false);
      std::vector<double> v;
      for (int k = 0; k < 100; ++k) {
        auto a = helix::monotonic_ns();
        e.price(m, r);
        v.push_back(static_cast<double>(helix::monotonic_ns() - a));
      }
      print(sm == helix::SyncMode::condition_variable ? "sync_condition"
                                                      : "sync_hybrid",
            stats(v));
    }
  };
  auto cache = [&] {
    struct Plain {
      std::atomic<std::uint64_t> value{};
    };
    struct alignas(64) Padded {
      std::atomic<std::uint64_t> value{};
    };
    auto run = []<class Counter>(std::array<Counter, 4> &c) {
      auto a = helix::monotonic_ns();
      std::array<std::jthread, 4> threads;
      for (std::size_t i = 0; i < 4; ++i)
        threads[i] = std::jthread([&, i] {
          for (int k = 0; k < 1'000'000; ++k)
            c[i].value.fetch_add(1, std::memory_order_relaxed);
        });
      for (auto &t : threads)
        t.join();
      return helix::monotonic_ns() - a;
    };
    std::array<Plain, 4> plain{};
    std::array<Padded, 4> padded{};
    auto unpadded = run(plain), padded_ns = run(padded);
    std::cout << "{\"benchmark\":\"false_sharing\",\"unpadded_ns\":" << unpadded
              << ",\"padded_ns\":" << padded_ns << ",\"speedup\":"
              << static_cast<double>(unpadded) / static_cast<double>(padded_ns)
              << "}\n";
  };
  if (mode == "all" || mode == "kernel")
    kernel();
  if (mode == "all" || mode == "scaling")
    scaling();
  if (mode == "all" || mode == "cdf")
    cdf();
  if (mode == "all" || mode == "iv")
    iv();
  if (mode == "all" || mode == "sync")
    sync();
  if (mode == "all" || mode == "cache")
    cache();
  return 0;
}
