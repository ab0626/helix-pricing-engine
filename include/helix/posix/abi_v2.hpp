#pragma once
#include "helix/core.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#ifdef __linux__
#include <pthread.h>
namespace helix::abi_v2 {
inline constexpr std::uint64_t magic = 0x48454c4958303031ULL;
inline constexpr std::uint32_t version = 3, result_capacity = 256,
                               batch_results = 64,
                               underlying_capacity = 64,
                               max_risk_consumers = 8;
enum class InitState : std::uint32_t { empty, initializing, ready, failed };
struct Header {
  std::uint64_t magic{};
  std::uint32_t abi{}, total_size{};
  std::atomic<std::uint32_t> state{};
  std::int32_t creator_pid{};
  std::uint64_t created_ns{};
  std::uint32_t capacity{};
  std::atomic<std::uint64_t> generation{}, errors{};
};
struct MarketRegion {
  Header header; pthread_mutex_t mutex{}; pthread_cond_t changed{};
  std::atomic<std::uint64_t> sequence{}, config_revision{};
  std::atomic<std::uint64_t> snapshots_published{}, reader_retries{},
      invalid_records{};
  std::atomic<std::uint32_t> shutdown_reason{};
  std::atomic<bool> shutdown{};
  MarketSnapshot snapshot;
};
struct ResultBatch {
  std::uint64_t market_generation{}, batch_id{}, first_instrument{}, count{1},
      market_publish_ns{}, price_complete_ns{}, validation_errors{};
  Result results[batch_results];
};
struct ResultRegion {
  Header header;
  alignas(64) std::atomic<std::uint64_t> write{};
  alignas(64) std::atomic<std::uint64_t> reads[max_risk_consumers]{};
  alignas(64) std::atomic<std::uint64_t> drops{};
  std::atomic<std::uint64_t> high_water{}, batches_published{},
      batches_consumed{}, options_priced{}, sequence_gaps{},
      ring_full_events{}, invalid_records{}, iv_failures{},
      calibration_failures{}, pricer_heartbeat_ns{};
  alignas(64) std::atomic<std::uint64_t> risk_heartbeat_ns[max_risk_consumers]{};
  std::atomic<std::uint64_t> worker_wakeups{};
  alignas(64) std::atomic<std::uint64_t> latency_count{}, latency_sum_ns{},
      latency_min_ns{}, latency_max_ns{};
  std::atomic<std::uint64_t> latency_histogram[64]{};
  std::atomic<std::uint32_t> worker_count{}, affinity_pins{}, realtime_workers{};
  std::atomic<std::uint32_t> shutdown_reason{};
  std::int32_t pricer_pid{}, risk_pids[max_risk_consumers]{}, numa_node{-1};
  std::atomic<bool> numa_applied{};
  std::uint32_t consumer_count{1}; char active_kernel[16]{};
  std::atomic<bool> shutdown{}; ResultBatch slots[result_capacity];
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::is_standard_layout_v<Header>);
static_assert(std::is_trivially_copyable_v<Result>);
static_assert(offsetof(ResultRegion, reads) % 64 == 0);
static_assert(offsetof(ResultRegion, drops) % 64 == 0);
static_assert(sizeof(ResultBatch::results) / sizeof(Result) == batch_results);
}
#endif
