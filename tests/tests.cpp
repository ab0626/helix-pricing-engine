#include "helix/core.hpp"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <random>
#include <vector>
namespace {
std::atomic<bool> track_allocations{false};
std::atomic<std::uint64_t> allocation_count{0};
} // namespace
void *operator new(std::size_t n) {
  if (track_allocations.load(std::memory_order_relaxed))
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void *p = std::malloc(n))
    return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
namespace {
int fails = 0;
void check(bool v, const char *n) {
  if (!v) {
    std::cerr << "FAIL " << n << '\n';
    ++fails;
  }
}
} // namespace
double cubic(double x, void *) noexcept { return x * x * x - 2; }
int main() {
  using namespace helix;
  Input x{100, 100, 1, .05, 0, .2, OptionType::call};
  auto c = black_scholes(x);
  check(std::abs(c.price - 10.450583572) < 1e-8, "known call");
  auto p = black_scholes({100, 100, 1, .05, 0, .2, OptionType::put});
  check(std::abs(c.price - p.price - (100 - 100 * std::exp(-.05))) < 1e-10,
        "parity");
  check(std::abs(c.delta - .636830651) < 1e-8, "delta");
  auto expiry = black_scholes({120, 100, 0, .05, .01, .2, OptionType::call});
  check(expiry.price == 20 && expiry.delta == 1, "expiry limit");
  auto deterministic = black_scholes({120, 100, 1, .05, .01, 0,
                                      OptionType::call});
  check(std::isfinite(deterministic.price) && deterministic.price > 0,
        "zero-volatility limit");
  check(std::isnan(black_scholes({-1, 100, 1, 0, 0, .2,
                                  OptionType::call}).price),
        "invalid pricing input rejected");
  check(implied_volatility(x, 200).status ==
            SolveStatus::price_outside_bounds,
        "IV price bounds");
  check(implied_volatility({100, 100, 0, 0, 0, .2, OptionType::call}, 1).status ==
            SolveStatus::invalid_input,
        "IV invalid expiry");
  {
    std::mt19937_64 random(12345);
    std::uniform_real_distribution<double> spots(40, 200), moneyness(.7, 1.3),
        vols(.1, .8), times(.1, 3), rates(-.03, .12);
    for (int sample = 0; sample < 300; ++sample) {
      auto spot = spots(random);
      Input z{spot, spot * moneyness(random), times(random), rates(random),
              rates(random) * .25, vols(random),
              sample & 1 ? OptionType::call : OptionType::put};
      auto analytic = black_scholes(z);
      double h = z.spot * 1e-4;
      auto up = z, down = z;
      up.spot += h; down.spot -= h;
      auto pu = black_scholes(up).price, pd = black_scholes(down).price;
      check(std::abs(analytic.delta - (pu - pd) / (2 * h)) < 2e-6,
            "random finite-difference delta");
      check(std::abs(analytic.gamma - (pu - 2 * analytic.price + pd) /
                                     (h * h)) < 2e-5,
            "random finite-difference gamma");
      auto solved = implied_volatility(z, analytic.price);
      check(solved.status == SolveStatus::converged &&
                std::abs(solved.value - z.volatility) < 2e-7,
            "random implied-volatility recovery");
    }
  }
  auto iv = implied_volatility(x, c.price),
       ivb = implied_volatility_bisection(x, c.price);
  check(iv.status == SolveStatus::converged && std::abs(iv.value - .2) < 1e-8,
        "iv newton");
  check(ivb.status == SolveStatus::converged && std::abs(ivb.value - .2) < 1e-8,
        "iv bisection");
  for (const auto &edge : std::array<Input, 4>{
           Input{100, 50, .05, .01, 0, .35, OptionType::call},
           Input{100, 150, .05, .01, 0, .35, OptionType::call},
           Input{100, 70, 2, -.01, .02, .08, OptionType::put},
           Input{100, 130, 5, .08, 0, 1.1, OptionType::put}}) {
    auto target = black_scholes(edge).price;
    auto solved = implied_volatility(edge, target, 1e-11, 200);
    check(solved.status == SolveStatus::converged &&
              std::abs(black_scholes({edge.spot, edge.strike, edge.time,
                                      edge.rate, edge.dividend, solved.value,
                                      edge.type}).price - target) < 1e-8,
          "edge-grid IV residual");
  }
  check(std::abs(brent(cubic, nullptr, 0, 2).value - std::cbrt(2.0)) < 1e-10,
        "brent");
  check(std::abs(black76(100, 100, 1, .05, .2, OptionType::call) -
                 7.577082146) < 1e-8,
        "black76");
  double maxerr = 0;
  for (int i = -800; i <= 800; ++i) {
    double z = i / 100.0;
    maxerr = std::max(maxerr, std::abs(normal_cdf(z) - fast_normal_cdf(z)));
  }
  check(maxerr < 8e-8, "fast cdf");
  SviParams truth{.02, .2, -.3, .01, .25};
  check(std::abs(svi_volatility(.1, 2, truth) -
                 std::sqrt(svi(.1, truth) / 2)) < 1e-14,
        "svi volatility interpolation");
  check(std::isnan(svi_volatility(.1, 0, truth)),
        "svi interpolation invalid expiry");
  std::vector<SviPoint> pts;
  for (int i = -10; i <= 10; ++i) {
    double k = i * .05;
    pts.push_back({k, svi(k, truth), 1});
  }
  auto fit = calibrate_svi(pts, 5000);
  check(fit.objective < 1e-6, "svi calibration");
  std::vector<SviPoint> noisy;
  for (int i = -20; i <= 20; ++i) {
    double k = i * .025;
    noisy.push_back({k, svi(k, truth) + 2e-4 * std::sin(i * 1.7), 1});
  }
  auto noisy_fit = calibrate_svi(noisy, 5000);
  check(noisy_fit.converged && noisy_fit.objective < 5e-6 &&
            valid(noisy_fit.params), "noisy svi calibration");
  std::vector<SviSliceData> surface_data;
  for (int expiry = 1; expiry <= 3; ++expiry) {
    SviSliceData slice; slice.expiry = .5 * expiry;
    auto scaled = truth; scaled.a *= expiry; scaled.b *= expiry;
    for (int i = -20; i <= 20; ++i) {
      double k = i * .025;
      slice.points.push_back({k, svi(k, scaled) + 1e-5 * std::sin(i), 1});
    }
    surface_data.push_back(std::move(slice));
  }
  auto surface = calibrate_svi_surface(surface_data, 5000);
  auto arbitrage = validate_svi_surface(surface);
  check(surface.slices.size() == 3 && arbitrage.valid &&
            arbitrage.minimum_calendar_spread >= -1e-10 &&
            arbitrage.minimum_density >= -1e-10,
        "calendar and butterfly constrained SVI surface");
  SviSurface crossed{{SviSlice{.5, {.1, .2, 0, 0, .2}, 0},
                      SviSlice{1.0, {.01, .02, 0, 0, .2}, 0}}};
  check(!validate_svi_surface(crossed).calendar_free,
        "calendar arbitrage detected");
  Matrix a{3, 2, {1, 1, 1, 2, 1, 3}};
  auto qr = qr_decompose(a);
  double orth = 0, recon = 0;
  for (std::size_t i = 0; i < 2; ++i)
    for (std::size_t j = 0; j < 2; ++j) {
      double z = 0;
      for (std::size_t k = 0; k < 3; ++k)
        z += qr.q(k, i) * qr.q(k, j);
      orth = std::max(orth, std::abs(z - (i == j)));
    }
  for (std::size_t i = 0; i < 3; ++i)
    for (std::size_t j = 0; j < 2; ++j) {
      double z = 0;
      for (std::size_t k = 0; k < 2; ++k)
        z += qr.q(i, k) * qr.r(k, j);
      recon = std::max(recon, std::abs(z - a(i, j)));
    }
  check(qr.success && orth < 1e-12 && recon < 1e-12, "qr");
  Matrix singular{3, 2, {1, 2, 2, 4, 3, 6}};
  check(!qr_decompose(singular).success, "qr rank deficiency detected");
  OptionSoA u;
  for (int i = 0; i < 64; ++i)
    u.push(i, 80 + i, 0.1 + i * .01, 0.01, 1,
           (i & 1) ? OptionType::put : OptionType::call);
  check(reinterpret_cast<std::uintptr_t>(u.strike.data()) % 64 == 0 &&
            reinterpret_cast<std::uintptr_t>(u.id.data()) % 64 == 0,
        "SoA arrays are cache-line aligned");
  std::vector<Result> out(u.size()), parallel(u.size());
  price_batch(u, MarketSnapshot{}, out);
  WorkerEngine engine(u, 4, SyncMode::hybrid_spin);
  engine.price(MarketSnapshot{}, parallel);
  for (std::size_t i = 0; i < u.size(); ++i)
    check(std::abs(out[i].greeks.price - parallel[i].greeks.price) < 1e-12,
          "workers parity");
  check(engine.wakeups() == 4, "worker completion");
  for (int generation = 0; generation < 100; ++generation) {
    MarketSnapshot snapshot{};
    snapshot.generation = static_cast<std::uint64_t>(generation + 1);
    engine.price(snapshot, parallel);
  }
  check(engine.wakeups() == 404, "repeated worker generations");
#ifdef HELIX_HAS_AVX2_KERNEL
  std::vector<Result> simd(u.size());
  if (avx2_available()) {
    check(price_batch_avx2(u, MarketSnapshot{}, simd), "avx execute");
    for (std::size_t i = 0; i < u.size(); ++i)
      check(std::abs(out[i].greeks.price - simd[i].greeks.price) < 1e-10,
            "avx parity");
    OptionSoA invalid;
    for (int i = 0; i < 4; ++i)
      invalid.push(i, i == 2 ? -1.0 : 100.0, i == 1 ? 0.0 : 1.0,
                   0.0, 1.0, OptionType::call);
    std::vector<Result> invalid_simd(4), invalid_scalar(4);
    price_batch(invalid, MarketSnapshot{}, invalid_scalar);
    check(price_batch_avx2(invalid, MarketSnapshot{}, invalid_simd),
          "avx invalid-lane execute");
    for (std::size_t i = 0; i < 4; ++i)
      check((std::isnan(invalid_scalar[i].greeks.price) &&
             std::isnan(invalid_simd[i].greeks.price)) ||
                std::abs(invalid_scalar[i].greeks.price -
                         invalid_simd[i].greeks.price) < 1e-7,
            "avx invalid-lane parity");
  }
#endif
#ifdef HELIX_HAS_AVX512_KERNEL
  if (avx512_available()) {
    std::vector<Result> simd512(u.size());
    check(price_batch_avx512(u, MarketSnapshot{}, simd512), "avx512 execute");
    for (std::size_t i = 0; i < u.size(); ++i)
      check(std::abs(out[i].greeks.price - simd512[i].greeks.price) < 1e-7 &&
                std::abs(out[i].greeks.delta - simd512[i].greeks.delta) < 1e-7,
            "avx512 parity");
  }
#endif
  auto id_capacity = u.id.capacity(), out_capacity = out.capacity();
  allocation_count.store(0);
  track_allocations.store(true);
  for (int repeat = 0; repeat < 100; ++repeat)
    price_batch(u, MarketSnapshot{}, out);
  track_allocations.store(false);
  check(allocation_count.load() == 0, "steady-state allocation free");
  check(u.id.capacity() == id_capacity && out.capacity() == out_capacity,
        "steady-state capacity");
  SpscRing<std::uint64_t, 4> ring;
  for (std::uint64_t round = 0; round < 10000; ++round) {
    for (std::uint64_t i = 0; i < 4; ++i)
      check(ring.push(round * 4 + i), "ring push");
    check(!ring.push(9), "ring full");
    for (std::uint64_t i = 0; i < 4; ++i) {
      std::uint64_t value = 0;
      check(ring.pop(value) && value == round * 4 + i, "ring wrap order");
    }
    std::uint64_t empty = 0;
    check(!ring.pop(empty), "ring empty");
  }
  check(ring.full_events() == 10000, "ring full-event accounting");
  {
    SeqlockSnapshot<MarketSnapshot> wrapped;
    wrapped.sequence.store(std::numeric_limits<std::uint64_t>::max() - 1);
    MarketSnapshot published{}; published.generation = 99;
    wrapped.publish(published);
    MarketSnapshot observed{};
    check(wrapped.sequence.load() == 0 && wrapped.read(observed) &&
              observed.generation == 99,
          "seqlock generation wraparound");
  }
  {
    const char *path = "helix_test_config.toml";
    {
      std::ofstream cfg(path);
      cfg << "workers = 3\nkernel = \"scalar-exact\"\nrealtime_priority = "
             "7\naffinity = false\n";
    }
    char program[] = "test", flag[] = "--config",
         file[] = "helix_test_config.toml", override_flag[] = "--workers",
         override_value[] = "4";
    char *argv[] = {program, flag, file, override_flag, override_value};
    auto parsed = parse_cli(5, argv);
    check(parsed.workers == 4 && parsed.kernel == "scalar-exact" &&
              parsed.realtime_priority == 7 && !parsed.affinity,
          "config and CLI override");
    std::remove(path);
  }
  {
    const char *path = "helix_bad_config.toml";
    { std::ofstream cfg(path); cfg << "workerz = 3\n"; }
    char program[] = "test", flag[] = "--config",
         file[] = "helix_bad_config.toml";
    char *argv[] = {program, flag, file};
    bool rejected = false;
    try { (void)parse_cli(3, argv); }
    catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "unknown config key rejected");
    std::remove(path);
  }
  std::cout << (fails ? "failed" : "all tests passed") << '\n';
  return fails ? 1 : 0;
}
