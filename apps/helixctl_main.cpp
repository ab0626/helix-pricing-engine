#include "helix/core.hpp"
#include "helix/common/version.hpp"
#include <iostream>
int main(int n, char **v) {
  if (n == 2 && std::string_view(v[1]) == "--version") { std::cout << "helixctl " << helix::version << '\n'; return 0; }
  if (n == 2 && std::string_view(v[1]) == "--help") { std::cout << "Usage: helixctl status|start|pause|resume|reload|shutdown [--socket PATH]\n"; return 0; }
  try {
    if (n < 2)
      throw std::invalid_argument("command required");
    std::string cmd = v[1];
    std::vector<char *> args{v[0]};
    for (int i = 2; i < n; ++i)
      args.push_back(v[i]);
    return helix::run_ctl(
        helix::parse_cli(static_cast<int>(args.size()), args.data()), cmd);
  } catch (const std::exception &e) {
    std::cerr << e.what()
              << "\nUsage: helixctl status|start|pause|resume|reload|shutdown "
                 "[--socket PATH]\n";
    return 2;
  }
}
