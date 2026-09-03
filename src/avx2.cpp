#include "helix/core.hpp"
#include <algorithm>
#include <cmath>
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace helix {
namespace {
__m256d cdf4(__m256d x) noexcept {
  const auto sign = _mm256_cmp_pd(x, _mm256_setzero_pd(), _CMP_LT_OQ);
  auto ax = _mm256_andnot_pd(_mm256_set1_pd(-0.0), x);
  auto t =
      _mm256_div_pd(_mm256_set1_pd(1),
                    _mm256_add_pd(_mm256_set1_pd(1),
                                  _mm256_mul_pd(_mm256_set1_pd(.2316419), ax)));
  auto poly = _mm256_set1_pd(1.330274429);
  poly = _mm256_add_pd(_mm256_set1_pd(-1.821255978), _mm256_mul_pd(t, poly));
  poly = _mm256_add_pd(_mm256_set1_pd(1.781477937), _mm256_mul_pd(t, poly));
  poly = _mm256_add_pd(_mm256_set1_pd(-.356563782), _mm256_mul_pd(t, poly));
  poly = _mm256_add_pd(_mm256_set1_pd(.319381530), _mm256_mul_pd(t, poly));
  alignas(32) double a[4], density[4];
  _mm256_store_pd(a, ax);
  for (int i = 0; i < 4; ++i)
    density[i] = std::exp(-.5 * a[i] * a[i]) / 2.5066282746310002;
  auto pos =
      _mm256_sub_pd(_mm256_set1_pd(1), _mm256_mul_pd(_mm256_load_pd(density),
                                                     _mm256_mul_pd(t, poly)));
  return _mm256_blendv_pd(pos, _mm256_sub_pd(_mm256_set1_pd(1), pos), sign);
}
} // namespace
bool avx2_available() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  int regs[4]{};
  __cpuid(regs, 0);
  if (regs[0] < 7)
    return false;
  __cpuidex(regs, 7, 0);
  return (regs[1] & (1 << 5)) != 0;
#elif defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}
bool price_batch_avx2(const OptionSoA &u, const MarketSnapshot &m,
                      std::span<Result> out) noexcept {
  if (!avx2_available() ||
      m.changed_underlying != std::numeric_limits<std::uint32_t>::max())
    return false;
  std::size_t n = std::min(u.size(), out.size()), i = 0;
  const auto spot = _mm256_set1_pd(m.spot), rate = _mm256_set1_pd(m.rate),
             vol = _mm256_set1_pd(m.volatility);
  for (; i + 4 <= n; i += 4) {
    alignas(32) double logsk[4], root[4], dqv[4], drv[4], signv[4];
    bool valid_group = m.spot > 0 && m.volatility > 0 &&
                       std::isfinite(m.spot + m.rate + m.volatility);
    for (int j = 0; j < 4; ++j) {
      auto k = u.strike[i + j], t = u.time[i + j];
      valid_group = valid_group && k > 0 && t > 0 &&
                    std::isfinite(k + t + u.dividend[i + j]);
      logsk[j] = std::log(m.spot / k);
      root[j] = std::sqrt(t);
      dqv[j] = std::exp(-u.dividend[i + j] * t);
      drv[j] = std::exp(-m.rate * t);
      signv[j] = u.type[i + j] == OptionType::call ? 1 : -1;
    }
    if (!valid_group) {
      for (int j = 0; j < 4; ++j)
        out[i + j] = {u.id[i + j], u.underlying_id[i + j],
                      u.quantity[i + j],
                      black_scholes({m.spot, u.strike[i + j], u.time[i + j],
                                     m.rate, u.dividend[i + j], m.volatility,
                                     u.type[i + j]}, true)};
      continue;
    }
    auto time = _mm256_loadu_pd(u.time.data() + i),
         div = _mm256_loadu_pd(u.dividend.data() + i),
         strike = _mm256_loadu_pd(u.strike.data() + i),
         roots = _mm256_load_pd(root), sign = _mm256_load_pd(signv);
    auto drift = _mm256_add_pd(
        _mm256_sub_pd(rate, div),
        _mm256_mul_pd(_mm256_set1_pd(.5), _mm256_mul_pd(vol, vol)));
    auto d1 = _mm256_div_pd(
             _mm256_add_pd(_mm256_load_pd(logsk), _mm256_mul_pd(drift, time)),
             _mm256_mul_pd(vol, roots)),
         d2 = _mm256_sub_pd(d1, _mm256_mul_pd(vol, roots));
    auto nd1 = cdf4(_mm256_mul_pd(sign, d1)),
         nd2 = cdf4(_mm256_mul_pd(sign, d2));
    auto price = _mm256_mul_pd(
        sign,
        _mm256_sub_pd(
            _mm256_mul_pd(spot, _mm256_mul_pd(_mm256_load_pd(dqv), nd1)),
            _mm256_mul_pd(strike, _mm256_mul_pd(_mm256_load_pd(drv), nd2))));
    auto delta = _mm256_mul_pd(sign, _mm256_mul_pd(_mm256_load_pd(dqv), nd1));
    alignas(32) double pv[4], dv[4], d1v[4], d2v[4];
    _mm256_store_pd(pv, price);
    _mm256_store_pd(dv, delta);
    _mm256_store_pd(d1v, d1);
    _mm256_store_pd(d2v, d2);
    for (int j = 0; j < 4; ++j) {
      double t = u.time[i + j],
             pdf = std::exp(-.5 * d1v[j] * d1v[j]) / 2.5066282746310002,
             s = signv[j], k = u.strike[i + j], q = u.dividend[i + j];
      Greeks g;
      g.price = pv[j];
      g.delta = dv[j];
      g.gamma = dqv[j] * pdf / (m.spot * m.volatility * root[j]);
      g.vega = m.spot * dqv[j] * pdf * root[j];
      g.theta = -(m.spot * dqv[j] * pdf * m.volatility) / (2 * root[j]) +
                s * (q * m.spot * dqv[j] * fast_normal_cdf(s * d1v[j]) -
                     m.rate * k * drv[j] * fast_normal_cdf(s * d2v[j]));
      g.rho = s * k * t * drv[j] * fast_normal_cdf(s * d2v[j]);
      out[i + j] = {u.id[i + j], u.underlying_id[i + j], u.quantity[i + j], g};
    }
  }
  for (; i < n; ++i)
    out[i] = {u.id[i], u.underlying_id[i], u.quantity[i],
              black_scholes({m.spot, u.strike[i], u.time[i], m.rate,
                             u.dividend[i], m.volatility, u.type[i]},
                            true)};
  return true;
}
} // namespace helix
