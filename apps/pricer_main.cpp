#include "helix/core.hpp"
#include "helix/common/version.hpp"
#include <iostream>
int main(int n, char **v) {
  if (n == 2 && std::string_view(v[1]) == "--version") { std::cout << "helix_pricer " << helix::version << '\n'; return 0; }
  if (n == 2 && std::string_view(v[1]) == "--help") { std::cout << "Usage: helix_pricer [--config FILE] [--market NAME] [--results NAME] [--universe N] [--workers N]\n"; return 0; }
  try {
    return helix::run_pricer(helix::parse_cli(n, v));
  } catch (const std::exception &e) {
    std::cerr << e.what()
              << "\nUsage: helix_pricer [--market NAME] [--results NAME] "
                 "[--universe N]\n";
    return 2;
  }
}
