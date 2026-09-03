#include "helix/core.hpp"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <new>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
namespace {
struct Record { std::uint64_t generation{}, inverse{}, square{}; };
struct Shared { helix::SeqlockSnapshot<Record> snapshot; std::atomic<bool> done{}; };
}
int main() {
  void *memory = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) return 2;
  auto *shared = new (memory) Shared{};
  pid_t child = fork();
  if (child == 0) {
    for (std::uint64_t i = 1; i <= 500000; ++i)
      shared->snapshot.publish({i, ~i, i * i});
    shared->done.store(true, std::memory_order_release);
    _exit(0);
  }
  std::uint64_t reads = 0, retries = 0;
  while (!shared->done.load(std::memory_order_acquire)) {
    Record r;
    if (!shared->snapshot.read(r)) { ++retries; continue; }
    if (r.generation &&
        (r.inverse != ~r.generation || r.square != r.generation * r.generation)) {
      std::cerr << "torn snapshot observed\n"; return 1;
    }
    ++reads;
  }
  int status = 0; waitpid(child, &status, 0); munmap(memory, sizeof(Shared));
  std::cout << "seqlock reads=" << reads << " retries=" << retries << '\n';
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
