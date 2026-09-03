#include "helix/core.hpp"
#include <chrono>
#ifdef __linux__
#include <time.h>
#endif
namespace helix {
std::uint64_t monotonic_ns() noexcept {
#ifdef __linux__
  timespec timestamp{};
#ifdef CLOCK_MONOTONIC_RAW
  clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp);
#else
  clock_gettime(CLOCK_MONOTONIC, &timestamp);
#endif
  return static_cast<std::uint64_t>(timestamp.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(timestamp.tv_nsec);
#else
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
} // namespace helix
