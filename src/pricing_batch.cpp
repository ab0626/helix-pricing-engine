#include "helix/core.hpp"
#include <algorithm>
namespace helix {
void OptionSoA::reserve(std::size_t count) {
  id.reserve(count); underlying_id.reserve(count); strike.reserve(count);
  time.reserve(count); dividend.reserve(count); quantity.reserve(count);
  type.reserve(count);
}
void OptionSoA::push(std::uint64_t instrument, double strike_value,
                     double expiry, double yield, double position,
                     OptionType option_type, std::uint32_t underlying) {
  id.push_back(instrument); underlying_id.push_back(underlying);
  strike.push_back(strike_value); time.push_back(expiry);
  dividend.push_back(yield); quantity.push_back(position);
  type.push_back(option_type);
}
void price_batch(const OptionSoA &universe, const MarketSnapshot &market,
                 std::span<Result> output, bool fast) noexcept {
  auto count = std::min(universe.size(), output.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (market.changed_underlying !=
            std::numeric_limits<std::uint32_t>::max() &&
        universe.underlying_id[i] != market.changed_underlying)
      continue;
    auto spot = market.underlying_spots[
        universe.underlying_id[i] % market.underlying_spots.size()];
    output[i] = {universe.id[i], universe.underlying_id[i], universe.quantity[i],
      black_scholes({spot, universe.strike[i], universe.time[i], market.rate,
                     universe.dividend[i], market.volatility, universe.type[i]},
                    fast)};
  }
}
} // namespace helix
