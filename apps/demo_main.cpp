#include "helix/core.hpp"
#include "helix/common/version.hpp"
#include <iostream>
int main(int n, char **v) {
  if (n == 2 && std::string_view(v[1]) == "--version") { std::cout << "helix_demo " << helix::version << '\n'; return 0; }
  if (n == 2 && std::string_view(v[1]) == "--help") { std::cout << "Usage: helix_demo [--duration N] [--universe N] [--workers N] [runtime options]\n"; return 0; }
  try {
    return helix::run_demo(helix::parse_cli(n, v));
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
