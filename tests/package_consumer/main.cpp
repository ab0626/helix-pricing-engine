#include <helix/pricing/models.hpp>
#include <cmath>
int main() {
  helix::Input input{100, 100, 1, .05, 0, .2, helix::OptionType::call};
  return std::abs(helix::black_scholes(input).price - 10.450583572) < 1e-8
             ? 0
             : 1;
}
