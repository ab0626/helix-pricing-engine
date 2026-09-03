#include "helix/core.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#ifdef __linux__
#include <time.h>
#include <unistd.h>
#endif

namespace helix {
namespace {
constexpr double pi = 3.14159265358979323846;
double pdf(double x) { return std::exp(-0.5 * x * x) / std::sqrt(2 * pi); }
} // namespace
double normal_cdf(double x) noexcept {
  return 0.5 * std::erfc(-x / std::sqrt(2.0));
}
double fast_normal_cdf(double x) noexcept {
  const double ax = std::abs(x), t = 1.0 / (1.0 + 0.2316419 * ax);
  const double p =
      1.0 -
      pdf(ax) * t *
          (0.319381530 +
           t * (-0.356563782 +
                t * (1.781477937 + t * (-1.821255978 + t * 1.330274429))));
  return x < 0 ? 1.0 - p : p;
}
Greeks black_scholes(const Input &x, bool fast) noexcept {
  Greeks g{};
  if (!(x.spot > 0 && x.strike > 0 && x.time >= 0 && x.volatility >= 0) ||
      !std::isfinite(x.spot + x.strike + x.time + x.rate + x.dividend +
                     x.volatility)) {
    g.price = std::numeric_limits<double>::quiet_NaN();
    return g;
  }
  const double sign = x.type == OptionType::call ? 1.0 : -1.0;
  if (x.time == 0) {
    g.price = std::max(sign * (x.spot - x.strike), 0.0);
    g.delta = sign * (sign * (x.spot - x.strike) > 0);
    return g;
  }
  const double dr = std::exp(-x.rate * x.time),
               dq = std::exp(-x.dividend * x.time);
  if (x.volatility == 0) {
    const double f = x.spot * std::exp((x.rate - x.dividend) * x.time);
    g.price = dr * std::max(sign * (f - x.strike), 0.0);
    g.delta = sign * dq * (sign * (f - x.strike) > 0);
    return g;
  }
  const double root = std::sqrt(x.time),
               d1 = (std::log(x.spot / x.strike) +
                     (x.rate - x.dividend + 0.5 * x.volatility * x.volatility) *
                         x.time) /
                    (x.volatility * root),
               d2 = d1 - x.volatility * root;
  auto cdf = fast ? fast_normal_cdf : normal_cdf;
  g.price =
      sign * (x.spot * dq * cdf(sign * d1) - x.strike * dr * cdf(sign * d2));
  g.delta = sign * dq * cdf(sign * d1);
  g.gamma = dq * pdf(d1) / (x.spot * x.volatility * root);
  g.vega = x.spot * dq * pdf(d1) * root;
  g.theta = -(x.spot * dq * pdf(d1) * x.volatility) / (2 * root) +
            sign * (x.dividend * x.spot * dq * cdf(sign * d1) -
                    x.rate * x.strike * dr * cdf(sign * d2));
  g.rho = sign * x.strike * x.time * dr * cdf(sign * d2);
  return g;
}
double black76(double f, double k, double t, double r, double v,
               OptionType type) noexcept {
  if (!(f > 0 && k > 0 && t >= 0 && v >= 0))
    return std::numeric_limits<double>::quiet_NaN();
  double s = type == OptionType::call ? 1 : -1;
  if (t == 0 || v == 0)
    return std::exp(-r * t) * std::max(s * (f - k), 0.0);
  double q = v * std::sqrt(t), d1 = std::log(f / k) / q + 0.5 * q, d2 = d1 - q;
  return std::exp(-r * t) * s *
         (f * normal_cdf(s * d1) - k * normal_cdf(s * d2));
}
SolveResult implied_volatility(Input x, double price, double tol,
                               int limit) noexcept {
  if (!(x.spot > 0 && x.strike > 0 && x.time > 0 && price >= 0))
    return {};
  double dq = std::exp(-x.dividend * x.time), dr = std::exp(-x.rate * x.time),
         loBound = x.type == OptionType::call
                       ? std::max(x.spot * dq - x.strike * dr, 0.0)
                       : std::max(x.strike * dr - x.spot * dq, 0.0),
         hiBound = x.type == OptionType::call ? x.spot * dq : x.strike * dr;
  if (price < loBound - tol || price > hiBound + tol)
    return {0, 0, 0, SolveStatus::price_outside_bounds};
  double lo = 1e-9, hi = 5.0, sigma = 0.2, res = 0;
  for (int i = 1; i <= limit; ++i) {
    x.volatility = sigma;
    auto g = black_scholes(x);
    res = g.price - price;
    if (std::abs(res) < tol)
      return {sigma, res, i, SolveStatus::converged};
    if (res > 0)
      hi = sigma;
    else
      lo = sigma;
    double n = sigma - res / g.vega;
    sigma = (g.vega > 1e-14 && n > lo && n < hi) ? n : 0.5 * (lo + hi);
  }
  return {sigma, res, limit, SolveStatus::max_iterations};
}
SolveResult bisect(RootFunction f, void *ctx, double lo, double hi, double tol,
                   int limit) noexcept {
  if (!f || !(lo < hi) || tol <= 0 || limit < 1)
    return {};
  double a = f(lo, ctx), b = f(hi, ctx);
  if (!std::isfinite(a + b))
    return {};
  if (a == 0)
    return {lo, 0, 0, SolveStatus::converged};
  if (b == 0)
    return {hi, 0, 0, SolveStatus::converged};
  if (std::signbit(a) == std::signbit(b))
    return {0, 0, 0, SolveStatus::not_bracketed};
  double mid = lo, res = a;
  for (int i = 1; i <= limit; ++i) {
    mid = std::midpoint(lo, hi);
    res = f(mid, ctx);
    if (std::abs(res) <= tol || (hi - lo) * .5 <= tol)
      return {mid, res, i, SolveStatus::converged};
    if (std::signbit(res) == std::signbit(a)) {
      lo = mid;
      a = res;
    } else
      hi = mid;
  }
  return {mid, res, limit, SolveStatus::max_iterations};
}
SolveResult brent(RootFunction f, void *ctx, double lo, double hi, double tol,
                  int limit) noexcept {
  if (!f || !(lo < hi) || tol <= 0 || limit < 1)
    return {};
  double a = lo, b = hi, fa = f(a, ctx), fb = f(b, ctx);
  if (!std::isfinite(fa + fb))
    return {};
  if (std::signbit(fa) == std::signbit(fb))
    return {0, 0, 0, SolveStatus::not_bracketed};
  if (std::abs(fa) < std::abs(fb)) {
    std::swap(a, b);
    std::swap(fa, fb);
  }
  double c = a, fc = fa, d = c;
  bool bis = true;
  for (int i = 1; i <= limit; ++i) {
    double s;
    if (fa != fc && fb != fc)
      s = a * fb * fc / ((fa - fb) * (fa - fc)) +
          b * fa * fc / ((fb - fa) * (fb - fc)) +
          c * fa * fb / ((fc - fa) * (fc - fb));
    else
      s = b - fb * (b - a) / (fb - fa);
    double lower = std::min((3 * a + b) * .25, b),
           upper = std::max((3 * a + b) * .25, b);
    bool reject = s < lower || s > upper ||
                  (bis && std::abs(s - b) >= std::abs(b - c) * .5) ||
                  (!bis && std::abs(s - b) >= std::abs(c - d) * .5) ||
                  (bis && std::abs(b - c) < tol) ||
                  (!bis && std::abs(c - d) < tol);
    if (reject) {
      s = (a + b) * .5;
      bis = true;
    } else
      bis = false;
    double fs = f(s, ctx);
    d = c;
    c = b;
    fc = fb;
    if (std::signbit(fa) != std::signbit(fs)) {
      b = s;
      fb = fs;
    } else {
      a = s;
      fa = fs;
    }
    if (std::abs(fa) < std::abs(fb)) {
      std::swap(a, b);
      std::swap(fa, fb);
    }
    if (std::abs(fb) <= tol || std::abs(b - a) <= tol)
      return {b, fb, i, SolveStatus::converged};
  }
  return {b, fb, limit, SolveStatus::max_iterations};
}
namespace {
struct IvCtx {
  Input x;
  double price;
};
double iv_fn(double v, void *p) noexcept {
  auto *c = static_cast<IvCtx *>(p);
  c->x.volatility = v;
  return black_scholes(c->x).price - c->price;
}
} // namespace
SolveResult implied_volatility_bisection(Input x, double price, double tol,
                                         int limit) noexcept {
  if (!(x.spot > 0 && x.strike > 0 && x.time > 0 && price >= 0))
    return {};
  double dq = std::exp(-x.dividend * x.time), dr = std::exp(-x.rate * x.time),
         lb = x.type == OptionType::call
                  ? std::max(x.spot * dq - x.strike * dr, 0.0)
                  : std::max(x.strike * dr - x.spot * dq, 0.0),
         ub = x.type == OptionType::call ? x.spot * dq : x.strike * dr;
  if (price < lb - tol || price > ub + tol)
    return {0, 0, 0, SolveStatus::price_outside_bounds};
  IvCtx ctx{x, price};
  return bisect(iv_fn, &ctx, 1e-9, 5.0, tol, limit);
}
bool valid(const SviParams &p) noexcept {
  return p.b >= 0 && std::abs(p.rho) < 1 && p.sigma > 0 &&
         std::isfinite(p.a + p.b + p.rho + p.m + p.sigma);
}
double svi(double k, const SviParams &p) noexcept {
  double d = k - p.m;
  return p.a + p.b * (p.rho * d + std::sqrt(d * d + p.sigma * p.sigma));
}
double svi_volatility(double k, double time, const SviParams &p) noexcept {
  if (!(time > 0) || !valid(p))
    return std::numeric_limits<double>::quiet_NaN();
  return std::sqrt(std::max(0.0, svi(k, p)) / time);
}
Calibration calibrate_svi(std::span<const SviPoint> pts, int limit) noexcept {
  using V = std::array<double, 5>;
  auto params = [](const V &v) {
    return SviParams{v[0], v[1], v[2], v[3], v[4]};
  };
  auto obj = [&](const V &v) {
    auto q = params(v);
    if (!valid(q) || q.b > 5 || q.sigma > 5 || std::abs(q.m) > 5 ||
        std::abs(q.a) > 5)
      return 1e100;
    double z = 0;
    for (auto &x : pts) {
      double e = svi(x.k, q) - x.variance;
      z += x.weight * e * e;
    }
    return z;
  };
  std::array<V, 6> x{};
  std::array<double, 6> fx{};
  x[0] = {.02, .2, -.2, 0, .2};
  const V step = {.02, .05, .1, .05, .05};
  for (std::size_t i = 1; i < x.size(); ++i) {
    x[i] = x[0];
    x[i][i - 1] += step[i - 1];
  }
  for (std::size_t i = 0; i < x.size(); ++i)
    fx[i] = obj(x[i]);
  int it = 0;
  bool converged = false;
  for (; it < limit; ++it) {
    std::array<std::size_t, 6> order{0, 1, 2, 3, 4, 5};
    std::sort(order.begin(), order.end(),
              [&](auto a, auto b) { return fx[a] < fx[b]; });
    std::array<V, 6> sx{};
    std::array<double, 6> sf{};
    for (std::size_t i = 0; i < 6; ++i) {
      sx[i] = x[order[i]];
      sf[i] = fx[order[i]];
    }
    x = sx;
    fx = sf;
    double spread = 0;
    for (double f : fx)
      spread = std::max(spread, std::abs(f - fx[0]));
    if (spread < 1e-16) {
      converged = true;
      break;
    }
    V c{};
    for (std::size_t i = 0; i < 5; ++i)
      for (std::size_t j = 0; j < 5; ++j)
        c[j] += x[i][j] / 5.0;
    auto combine = [&](const V &a, const V &b, double scale) {
      V v{};
      for (std::size_t j = 0; j < 5; ++j)
        v[j] = a[j] + scale * (a[j] - b[j]);
      return v;
    };
    V reflected = combine(c, x[5], 1);
    double fr = obj(reflected);
    if (fr < fx[0]) {
      V expanded = combine(c, x[5], 2);
      double fe = obj(expanded);
      x[5] = fe < fr ? expanded : reflected;
      fx[5] = std::min(fe, fr);
    } else if (fr < fx[4]) {
      x[5] = reflected;
      fx[5] = fr;
    } else {
      V contracted{};
      if (fr < fx[5])
        for (std::size_t j = 0; j < 5; ++j)
          contracted[j] = c[j] + .5 * (reflected[j] - c[j]);
      else
        for (std::size_t j = 0; j < 5; ++j)
          contracted[j] = c[j] + .5 * (x[5][j] - c[j]);
      double fc = obj(contracted);
      if (fc < std::min(fr, fx[5])) {
        x[5] = contracted;
        fx[5] = fc;
      } else
        for (std::size_t i = 1; i < 6; ++i) {
          for (std::size_t j = 0; j < 5; ++j)
            x[i][j] = x[0][j] + .5 * (x[i][j] - x[0][j]);
          fx[i] = obj(x[i]);
        }
    }
  }
  auto best = static_cast<std::size_t>(std::min_element(fx.begin(), fx.end()) -
                                       fx.begin());
  return {params(x[best]), fx[best], it, converged};
}
ArbitrageCheck validate_svi_surface(const SviSurface &surface, double lo,
                                    double hi, std::size_t grid) noexcept {
  ArbitrageCheck check{true, true, true,
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::infinity()};
  if (surface.slices.empty() || !(lo < hi) || grid < 3) return {};
  for (std::size_t slice = 0; slice < surface.slices.size(); ++slice) {
    const auto &entry = surface.slices[slice];
    if (!(entry.expiry > 0) || !valid(entry.params) ||
        (slice && entry.expiry <= surface.slices[slice - 1].expiry)) return {};
    for (std::size_t point = 0; point < grid; ++point) {
      double k = lo + (hi - lo) * static_cast<double>(point) /
                           static_cast<double>(grid - 1);
      const auto &p = entry.params; double x = k - p.m;
      double root = std::sqrt(x * x + p.sigma * p.sigma);
      double w = svi(k, p), first = p.b * (p.rho + x / root);
      double second = p.b * p.sigma * p.sigma / (root * root * root);
      if (!(w > 0) || !std::isfinite(w + first + second)) return {};
      double density = std::pow(1 - k * first / (2 * w), 2) -
          first * first * (.25 / w + .0625) + .5 * second;
      check.minimum_density = std::min(check.minimum_density, density);
      if (density < -1e-10) check.butterfly_free = false;
      if (slice) {
        double spread = w - svi(k, surface.slices[slice - 1].params);
        check.minimum_calendar_spread =
            std::min(check.minimum_calendar_spread, spread);
        if (spread < -1e-10) check.calendar_free = false;
      }
    }
  }
  if (surface.slices.size() == 1) check.minimum_calendar_spread = 0;
  check.valid = check.calendar_free && check.butterfly_free;
  return check;
}
SviSurface calibrate_svi_surface(std::span<const SviSliceData> data,
                                 int iterations) noexcept {
  SviSurface surface;
  std::vector<std::size_t> order(data.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](auto a, auto b) {
    return data[a].expiry < data[b].expiry;
  });
  for (auto index : order) {
    auto fit = calibrate_svi(data[index].points, iterations);
    SviSlice slice{data[index].expiry, fit.params, fit.objective};
    if (!surface.slices.empty()) {
      double lift = 0;
      for (int point = 0; point <= 1200; ++point) {
        double k = -3.0 + 6.0 * point / 1200.0;
        lift = std::max(lift, svi(k, surface.slices.back().params) -
                                  svi(k, slice.params));
      }
      slice.params.a += lift + 1e-10;
    }
    surface.slices.push_back(slice);
    for (int repair = 0; repair < 200; ++repair) {
      auto check = validate_svi_surface(surface);
      if (check.valid) break;
      surface.slices.back().params.a += .001;
    }
  }
  return surface;
}
QrResult qr_decompose(const Matrix &a, double eps) {
  QrResult z;
  z.q = {a.rows, a.cols, std::vector<double>(a.rows * a.cols)};
  z.r = {a.cols, a.cols, std::vector<double>(a.cols * a.cols)};
  if (a.rows < a.cols || a.values.size() != a.rows * a.cols)
    return z;
  std::vector<double> v(a.rows);
  for (std::size_t j = 0; j < a.cols; ++j) {
    for (std::size_t r = 0; r < a.rows; ++r)
      v[r] = a(r, j);
    for (std::size_t k = 0; k < j; ++k) {
      double dot = 0;
      for (std::size_t r = 0; r < a.rows; ++r)
        dot += z.q(r, k) * v[r];
      z.r(k, j) = dot;
      for (std::size_t r = 0; r < a.rows; ++r)
        v[r] -= dot * z.q(r, k);
    }
    double norm = 0;
    for (double x : v)
      norm += x * x;
    norm = std::sqrt(norm);
    if (norm <= eps)
      return z;
    z.r(j, j) = norm;
    for (std::size_t r = 0; r < a.rows; ++r)
      z.q(r, j) = v[r] / norm;
  }
  z.success = true;
  return z;
}
#ifndef HELIX_HAS_AVX2_KERNEL
bool avx2_available() noexcept { return false; }
bool price_batch_avx2(const OptionSoA &, const MarketSnapshot &,
                      std::span<Result>) noexcept {
  return false;
}
#endif
#ifndef HELIX_HAS_AVX512_KERNEL
bool avx512_available() noexcept { return false; }
bool price_batch_avx512(const OptionSoA &, const MarketSnapshot &,
                        std::span<Result>) noexcept { return false; }
#endif
Cli parse_cli(int argc, char **argv) {
  Cli c;
#ifdef __linux__
  c.socket_path = "/tmp/helix_" + std::to_string(getuid()) + ".sock";
#else
  c.socket_path = "/tmp/helix.sock";
#endif
  auto unquote = [](std::string v) {
    auto first = v.find_first_not_of(" \t"), last = v.find_last_not_of(" \t\r");
    v = first == std::string::npos ? "" : v.substr(first, last - first + 1);
    if (v.size() > 1 && v.front() == '"' && v.back() == '"')
      v = v.substr(1, v.size() - 2);
    return v;
  };
  auto set = [&](const std::string &key, const std::string &raw) {
    auto v = unquote(raw);
    if (key == "market_name")
      c.market_name = v;
    else if (key == "result_name")
      c.result_name = v;
    else if (key == "socket_path")
      c.socket_path = v;
    else if (key == "universe_size")
      c.universe = std::stoull(v);
    else if (key == "ring_capacity")
      c.ring_capacity = std::stoull(v);
    else if (key == "underlyings")
      c.underlyings = std::stoull(v);
    else if (key == "workers")
      c.workers = std::stoi(v);
    else if (key == "update_hz")
      c.update_hz = std::stoi(v);
    else if (key == "kernel")
      c.kernel = v;
    else if (key == "sync")
      c.sync = v;
    else if (key == "spin_ns")
      c.spin_ns = std::stoull(v);
    else if (key == "seed")
      c.seed = std::stoull(v);
    else if (key == "affinity")
      c.affinity = v == "true";
    else if (key == "realtime_priority")
      c.realtime_priority = std::stoi(v);
    else if (key == "risk_consumers")
      c.risk_consumers = std::stoi(v);
    else if (key == "consumer_id")
      c.consumer_id = std::stoi(v);
    else if (key == "numa_node")
      c.numa_node = std::stoi(v);
    else if (key == "duration")
      c.duration = std::stoi(v);
    else if (key == "drop_policy")
      c.drop_policy = v;
    else if (key == "log_level")
      c.log_level = v;
    else if (key == "cleanup_policy")
      c.cleanup_policy = v;
    else if (key == "allowed_cpus")
      c.allowed_cpus = v;
    else
      throw std::invalid_argument("unknown config key: " + key);
  };
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--config") {
      c.config_path = argv[i + 1];
      std::ifstream in(c.config_path);
      if (!in)
        throw std::invalid_argument("cannot open config: " + c.config_path);
      std::string line;
      while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos)
          line.resize(hash);
        auto eq = line.find('=');
        if (eq != std::string::npos)
          set(unquote(line.substr(0, eq)), line.substr(eq + 1));
      }
      break;
    }
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&]() {
      if (++i >= argc)
        throw std::invalid_argument("missing value for " + a);
      return std::string(argv[i]);
    };
    if (a == "--config") {
      c.config_path = val();
    } else if (a == "--market")
      c.market_name = val();
    else if (a == "--results")
      c.result_name = val();
    else if (a == "--socket")
      c.socket_path = val();
    else if (a == "--universe")
      c.universe = std::stoull(val());
    else if (a == "--ring-capacity")
      c.ring_capacity = std::stoull(val());
    else if (a == "--underlyings")
      c.underlyings = std::stoull(val());
    else if (a == "--workers")
      c.workers = std::stoi(val());
    else if (a == "--duration")
      c.duration = std::stoi(val());
    else if (a == "--update-hz")
      c.update_hz = std::stoi(val());
    else if (a == "--kernel")
      c.kernel = val();
    else if (a == "--sync")
      c.sync = val();
    else if (a == "--spin-ns")
      c.spin_ns = std::stoull(val());
    else if (a == "--seed")
      c.seed = std::stoull(val());
    else if (a == "--rt-priority")
      c.realtime_priority = std::stoi(val());
    else if (a == "--risk-consumers")
      c.risk_consumers = std::stoi(val());
    else if (a == "--consumer-id")
      c.consumer_id = std::stoi(val());
    else if (a == "--numa-node")
      c.numa_node = std::stoi(val());
    else if (a == "--drop-policy")
      c.drop_policy = val();
    else if (a == "--log-level")
      c.log_level = val();
    else if (a == "--cleanup-policy")
      c.cleanup_policy = val();
    else if (a == "--allowed-cpus")
      c.allowed_cpus = val();
    else if (a == "--no-affinity")
      c.affinity = false;
    else if (a == "--help")
      throw std::invalid_argument("help");
    else
      throw std::invalid_argument("unknown argument: " + a);
  }
  if (c.workers < 1 || c.workers > 8 || c.universe == 0 || c.update_hz < 1 ||
      c.update_hz > 100000 || c.realtime_priority < 0 ||
      c.realtime_priority > 99 || c.duration < 1 || c.ring_capacity < 2 ||
      c.ring_capacity > 256 || c.underlyings < 1 || c.underlyings > 8)
    throw std::invalid_argument("workers 1..8, positive universe, update-hz "
                                "1..100000, rt-priority 0..99, duration >= 1, "
                                "ring-capacity 2..256, underlyings 1..8 required");
  if (c.risk_consumers < 1 || c.risk_consumers > 8 || c.consumer_id < 0 ||
      c.consumer_id >= c.risk_consumers || c.numa_node < -1 || c.numa_node > 1023)
    throw std::invalid_argument("risk-consumers 1..8, consumer-id within range, "
                                "and numa-node -1..1023 required");
  if (c.kernel != "scalar-exact" && c.kernel != "scalar-fast" &&
      c.kernel != "avx2-fast" && c.kernel != "avx512-fast")
    throw std::invalid_argument(
        "kernel must be scalar-exact, scalar-fast, avx2-fast, or avx512-fast");
  if (c.sync != "condition" && c.sync != "hybrid")
    throw std::invalid_argument("sync must be condition or hybrid");
  if (c.drop_policy != "drop" && c.drop_policy != "block")
    throw std::invalid_argument("drop-policy must be drop or block");
  if (c.log_level != "trace" && c.log_level != "debug" &&
      c.log_level != "info" && c.log_level != "warn" &&
      c.log_level != "error")
    throw std::invalid_argument("invalid log-level");
  if (c.cleanup_policy != "owner" && c.cleanup_policy != "retain")
    throw std::invalid_argument("cleanup-policy must be owner or retain");
  if (!c.allowed_cpus.empty()) {
    std::istringstream input(c.allowed_cpus);
    for (std::string cpu; std::getline(input, cpu, ',');) {
      if (cpu.empty() || !std::all_of(cpu.begin(), cpu.end(), ::isdigit) ||
          std::stoull(cpu) >= 1024)
        throw std::invalid_argument("allowed-cpus must be comma-separated CPU IDs");
    }
  }
  return c;
}
} // namespace helix
