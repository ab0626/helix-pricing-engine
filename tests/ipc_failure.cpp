#include "helix/core.hpp"
#include <iostream>
int main() {
  bool passed = helix::run_ipc_failure_self_test();
  std::cout << (passed ? "IPC failure recovery passed"
                       : "IPC failure recovery failed")
            << '\n';
  return passed ? 0 : 1;
}
