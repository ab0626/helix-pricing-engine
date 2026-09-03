#include "helix/core.hpp"
#include <algorithm>
#include <cmath>
#include <immintrin.h>
namespace helix {
namespace {
__m512d cdf8(__m512d value) noexcept {
  auto absolute = _mm512_abs_pd(value);
  auto t = _mm512_div_pd(_mm512_set1_pd(1),
      _mm512_add_pd(_mm512_set1_pd(1),
                    _mm512_mul_pd(_mm512_set1_pd(.2316419), absolute)));
  auto polynomial = _mm512_set1_pd(1.330274429);
  polynomial = _mm512_add_pd(_mm512_set1_pd(-1.821255978), _mm512_mul_pd(t, polynomial));
  polynomial = _mm512_add_pd(_mm512_set1_pd(1.781477937), _mm512_mul_pd(t, polynomial));
  polynomial = _mm512_add_pd(_mm512_set1_pd(-.356563782), _mm512_mul_pd(t, polynomial));
  polynomial = _mm512_add_pd(_mm512_set1_pd(.319381530), _mm512_mul_pd(t, polynomial));
  alignas(64) double x[8], density[8]; _mm512_store_pd(x, absolute);
  for (int lane = 0; lane < 8; ++lane)
    density[lane] = std::exp(-.5 * x[lane] * x[lane]) / 2.5066282746310002;
  auto positive = _mm512_sub_pd(_mm512_set1_pd(1),
      _mm512_mul_pd(_mm512_load_pd(density), _mm512_mul_pd(t, polynomial)));
  auto negative = _mm512_sub_pd(_mm512_set1_pd(1), positive);
  return _mm512_mask_blend_pd(_mm512_cmp_pd_mask(value, _mm512_setzero_pd(), _CMP_LT_OQ),
                              positive, negative);
}
}
bool avx512_available() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("avx512f") &&
         __builtin_cpu_supports("avx512dq");
#else
  return false;
#endif
}
bool price_batch_avx512(const OptionSoA &universe, const MarketSnapshot &market,
                        std::span<Result> output) noexcept {
  if (!avx512_available() || market.changed_underlying !=
          std::numeric_limits<std::uint32_t>::max()) return false;
  auto count = std::min(universe.size(), output.size()); std::size_t i = 0;
  auto spot = _mm512_set1_pd(market.spot), rate = _mm512_set1_pd(market.rate),
       volatility = _mm512_set1_pd(market.volatility);
  for (; i + 8 <= count; i += 8) {
    alignas(64) double logsk[8], roots[8], dq[8], dr[8], signs[8];
    bool valid_group = market.spot > 0 && market.volatility > 0;
    for (int lane = 0; lane < 8; ++lane) {
      auto strike = universe.strike[i + lane], time = universe.time[i + lane];
      valid_group = valid_group && strike > 0 && time > 0 &&
                    std::isfinite(strike + time + universe.dividend[i + lane]);
      logsk[lane] = std::log(market.spot / strike); roots[lane] = std::sqrt(time);
      dq[lane] = std::exp(-universe.dividend[i + lane] * time);
      dr[lane] = std::exp(-market.rate * time);
      signs[lane] = universe.type[i + lane] == OptionType::call ? 1 : -1;
    }
    if (!valid_group) {
      for (int lane = 0; lane < 8; ++lane)
        output[i + lane] = {universe.id[i + lane], universe.underlying_id[i + lane],
          universe.quantity[i + lane], black_scholes({market.spot,
          universe.strike[i + lane], universe.time[i + lane], market.rate,
          universe.dividend[i + lane], market.volatility, universe.type[i + lane]}, true)};
      continue;
    }
    auto time = _mm512_load_pd(universe.time.data() + i);
    auto dividend = _mm512_load_pd(universe.dividend.data() + i);
    auto strike = _mm512_load_pd(universe.strike.data() + i);
    auto root = _mm512_load_pd(roots), sign = _mm512_load_pd(signs);
    auto drift = _mm512_add_pd(_mm512_sub_pd(rate, dividend),
        _mm512_mul_pd(_mm512_set1_pd(.5), _mm512_mul_pd(volatility, volatility)));
    auto d1 = _mm512_div_pd(_mm512_add_pd(_mm512_load_pd(logsk),
        _mm512_mul_pd(drift, time)), _mm512_mul_pd(volatility, root));
    auto d2 = _mm512_sub_pd(d1, _mm512_mul_pd(volatility, root));
    auto n1 = cdf8(_mm512_mul_pd(sign, d1)), n2 = cdf8(_mm512_mul_pd(sign, d2));
    auto price = _mm512_mul_pd(sign, _mm512_sub_pd(
        _mm512_mul_pd(spot, _mm512_mul_pd(_mm512_load_pd(dq), n1)),
        _mm512_mul_pd(strike, _mm512_mul_pd(_mm512_load_pd(dr), n2))));
    auto delta = _mm512_mul_pd(sign, _mm512_mul_pd(_mm512_load_pd(dq), n1));
    alignas(64) double prices[8], deltas[8];
    _mm512_store_pd(prices, price); _mm512_store_pd(deltas, delta);
    for (int lane = 0; lane < 8; ++lane) {
      auto greeks = black_scholes({market.spot, universe.strike[i + lane],
          universe.time[i + lane], market.rate, universe.dividend[i + lane],
          market.volatility, universe.type[i + lane]}, true);
      greeks.price = prices[lane];
      greeks.delta = deltas[lane];
      output[i + lane] = {universe.id[i + lane], universe.underlying_id[i + lane],
                          universe.quantity[i + lane], greeks};
    }
  }
  for (; i < count; ++i)
    output[i] = {universe.id[i], universe.underlying_id[i], universe.quantity[i],
      black_scholes({market.spot, universe.strike[i], universe.time[i], market.rate,
                     universe.dividend[i], market.volatility, universe.type[i]}, true)};
  return true;
}
} // namespace helix
