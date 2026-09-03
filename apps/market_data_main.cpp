#include "helix/core.hpp"
#include "helix/common/version.hpp"
#include <iostream>
int main(int n, char **v) {
  if (n == 2 && std::string_view(v[1]) == "--version") { std::cout << "helix_market_data " << helix::version << '\n'; return 0; }
  if (n == 2 && std::string_view(v[1]) == "--help") { std::cout << "Usage: helix_market_data [--config FILE] [--market NAME] [--socket PATH] [--update-hz N]\n"; return 0; }
  try {
    return helix::run_market_data(helix::parse_cli(n, v));
  } catch (const std::exception &e) {
    std::cerr << e.what()
              << "\nUsage: helix_market_data [--market NAME] [--socket PATH]\n";
    return 2;
  }
}
