#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace helix {
template <class T, std::size_t Alignment = 64> struct AlignedAllocator {
  using value_type = T;
  using is_always_equal = std::true_type;
  template <class U> struct rebind { using other = AlignedAllocator<U, Alignment>; };
  AlignedAllocator() noexcept = default;
  template <class U>
  AlignedAllocator(const AlignedAllocator<U, Alignment> &) noexcept {}
  [[nodiscard]] T *allocate(std::size_t count) {
    if (count > static_cast<std::size_t>(-1) / sizeof(T)) throw std::bad_array_new_length();
    return static_cast<T *>(::operator new(count * sizeof(T),
                                           std::align_val_t{Alignment}));
  }
  void deallocate(T *pointer, std::size_t) noexcept {
    ::operator delete(pointer, std::align_val_t{Alignment});
  }
};
template <class T, class U, std::size_t A>
bool operator==(const AlignedAllocator<T, A> &,
                const AlignedAllocator<U, A> &) noexcept { return true; }
enum class OptionType : std::uint8_t { call, put };
struct Input {
  double spot, strike, time, rate, dividend, volatility;
  OptionType type;
};
struct Greeks {
  double price{}, delta{}, gamma{}, vega{}, theta{}, rho{};
};
[[nodiscard]] double normal_cdf(double x) noexcept;
[[nodiscard]] double fast_normal_cdf(double x) noexcept;
[[nodiscard]] Greeks black_scholes(const Input &, bool fast = false) noexcept;
[[nodiscard]] double black76(double forward, double strike, double time,
                             double rate, double volatility,
                             OptionType) noexcept;

enum class SolveStatus {
  converged,
  invalid_input,
  price_outside_bounds,
  not_bracketed,
  derivative_too_small,
  max_iterations
};
struct SolveResult {
  double value{}, residual{};
  int iterations{};
  SolveStatus status{SolveStatus::invalid_input};
};
using RootFunction = double (*)(double, void *) noexcept;
[[nodiscard]] SolveResult bisect(RootFunction, void *, double lo, double hi,
                                 double tolerance = 1e-12,
                                 int max_iterations = 100) noexcept;
[[nodiscard]] SolveResult brent(RootFunction, void *, double lo, double hi,
                                double tolerance = 1e-12,
                                int max_iterations = 100) noexcept;
[[nodiscard]] SolveResult implied_volatility(Input base, double market_price,
                                             double tolerance = 1e-10,
                                             int max_iterations = 100) noexcept;
[[nodiscard]] SolveResult
implied_volatility_bisection(Input base, double market_price,
                             double tolerance = 1e-10,
                             int max_iterations = 100) noexcept;

struct SviParams {
  double a{}, b{}, rho{}, m{}, sigma{0.2};
};
[[nodiscard]] bool valid(const SviParams &) noexcept;
[[nodiscard]] double svi(double k, const SviParams &) noexcept;
[[nodiscard]] double svi_volatility(double k, double time,
                                    const SviParams &) noexcept;
struct SviPoint {
  double k, variance, weight{1.0};
};
struct Calibration {
  SviParams params;
  double objective{};
  int iterations{};
  bool converged{};
};
[[nodiscard]] Calibration calibrate_svi(std::span<const SviPoint>,
                                        int max_iterations = 1000) noexcept;
struct SviSliceData { double expiry{}; std::vector<SviPoint> points; };
struct SviSlice { double expiry{}; SviParams params; double objective{}; };
struct SviSurface { std::vector<SviSlice> slices; };
struct ArbitrageCheck {
  bool valid{}, calendar_free{}, butterfly_free{};
  double minimum_calendar_spread{}, minimum_density{};
};
[[nodiscard]] ArbitrageCheck validate_svi_surface(
    const SviSurface &, double min_log_moneyness = -3,
    double max_log_moneyness = 3, std::size_t grid_points = 1201) noexcept;
[[nodiscard]] SviSurface calibrate_svi_surface(
    std::span<const SviSliceData>, int max_iterations = 3000) noexcept;

struct Matrix {
  std::size_t rows{}, cols{};
  std::vector<double> values;
  double &operator()(std::size_t r, std::size_t c) {
    return values[r * cols + c];
  }
  double operator()(std::size_t r, std::size_t c) const {
    return values[r * cols + c];
  }
};
struct QrResult {
  Matrix q, r;
  bool success{};
};
[[nodiscard]] QrResult qr_decompose(const Matrix &, double epsilon = 1e-12);

struct alignas(64) MarketSnapshot {
  std::uint64_t generation{};
  std::uint64_t timestamp_ns{};
  double spot{100}, rate{0.03}, volatility{0.2};
  std::array<double, 8> underlying_spots{100, 100, 100, 100, 100, 100, 100,
                                         100};
  std::uint32_t changed_underlying{std::numeric_limits<std::uint32_t>::max()};
};
struct Result {
  std::uint64_t instrument_id{};
  std::uint32_t underlying_id{};
  double quantity{1.0};
  Greeks greeks;
};
using ResultBuffer = std::vector<Result, AlignedAllocator<Result>>;
template <class T> struct SeqlockSnapshot {
  static_assert(std::is_trivially_copyable_v<T>);
  std::atomic<std::uint64_t> sequence{};
  T value{};
  void publish(const T &next) noexcept {
    auto s = sequence.load(std::memory_order_relaxed);
    sequence.store(s + 1, std::memory_order_release);
    value = next;
    sequence.store(s + 2, std::memory_order_release);
  }
  [[nodiscard]] bool read(T &out, int retries = 1000) const noexcept {
    for (int i = 0; i < retries; ++i) {
      auto before = sequence.load(std::memory_order_acquire);
      if (before & 1U)
        continue;
      out = value;
      std::atomic_thread_fence(std::memory_order_acquire);
      if (before == sequence.load(std::memory_order_relaxed))
        return true;
    }
    return false;
  }
};
template <class T, std::size_t Capacity> class SpscRing {
  static_assert(Capacity > 1);

public:
  [[nodiscard]] bool push(const T &v) noexcept {
    auto w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) >= Capacity) {
      full_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slots_[w % Capacity] = v;
    write_.store(w + 1, std::memory_order_release);
    return true;
  }
  [[nodiscard]] bool pop(T &v) noexcept {
    auto r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire))
      return false;
    v = slots_[r % Capacity];
    read_.store(r + 1, std::memory_order_release);
    return true;
  }
  [[nodiscard]] std::uint64_t full_events() const noexcept {
    return full_.load();
  }

private:
  std::array<T, Capacity> slots_{};
  alignas(64) std::atomic<std::uint64_t> write_{};
  alignas(64) std::atomic<std::uint64_t> read_{};
  alignas(64) std::atomic<std::uint64_t> full_{};
};
struct OptionSoA {
  std::vector<std::uint64_t, AlignedAllocator<std::uint64_t>> id;
  std::vector<std::uint32_t, AlignedAllocator<std::uint32_t>> underlying_id;
  std::vector<double, AlignedAllocator<double>> strike, time, dividend, quantity;
  std::vector<OptionType, AlignedAllocator<OptionType>> type;
  void reserve(std::size_t);
  void push(std::uint64_t, double, double, double, double, OptionType,
            std::uint32_t underlying = 0);
  [[nodiscard]] std::size_t size() const noexcept { return id.size(); }
};
void price_batch(const OptionSoA &, const MarketSnapshot &,
                 std::span<Result>, bool fast = true) noexcept;
[[nodiscard]] bool avx2_available() noexcept;
[[nodiscard]] bool price_batch_avx2(const OptionSoA &, const MarketSnapshot &,
                                    std::span<Result>) noexcept;
[[nodiscard]] bool avx512_available() noexcept;
[[nodiscard]] bool price_batch_avx512(const OptionSoA &, const MarketSnapshot &,
                                      std::span<Result>) noexcept;

enum class SyncMode { condition_variable, hybrid_spin };
class WorkerEngine {
public:
  WorkerEngine(const OptionSoA &, std::size_t workers,
               SyncMode mode = SyncMode::condition_variable,
               std::uint64_t spin_ns = 50'000, bool affinity = true,
               int realtime_priority = 0, std::string allowed_cpus = {});
  ~WorkerEngine();
  WorkerEngine(const WorkerEngine &) = delete;
  WorkerEngine &operator=(const WorkerEngine &) = delete;
  void price(const MarketSnapshot &, std::span<Result>);
  [[nodiscard]] std::size_t workers() const noexcept;
  [[nodiscard]] std::uint64_t wakeups() const noexcept;
  [[nodiscard]] std::size_t affinity_pins() const noexcept;
  [[nodiscard]] std::size_t realtime_workers() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct Cli {
  std::string market_name{"/helix_market"}, result_name{"/helix_results"},
      socket_path{}, kernel{"scalar-fast"}, sync{"condition"},
      drop_policy{"drop"}, log_level{"info"}, cleanup_policy{"owner"},
      allowed_cpus{}, config_path{};
  std::size_t universe{4096}, ring_capacity{256}, underlyings{8};
  int workers{2}, duration{5}, update_hz{100}, realtime_priority{0},
      risk_consumers{1}, consumer_id{0}, numa_node{-1};
  std::uint64_t spin_ns{50'000}, seed{42};
  bool affinity{true};
};
[[nodiscard]] Cli parse_cli(int argc, char **argv);
[[nodiscard]] std::uint64_t monotonic_ns() noexcept;
int run_market_data(const Cli &);
int run_pricer(const Cli &);
int run_risk(const Cli &);
int run_ctl(const Cli &, const std::string &);
int run_demo(const Cli &);
[[nodiscard]] bool run_ipc_failure_self_test();
} // namespace helix
