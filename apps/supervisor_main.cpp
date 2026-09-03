#include "helix/core.hpp"
#include "helix/common/version.hpp"
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) noexcept { stopping = 1; }
std::string executable_directory() {
  char path[4096]{};
  auto n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n <= 0) throw std::runtime_error("cannot resolve supervisor executable");
  std::string value(path, static_cast<std::size_t>(n));
  return value.substr(0, value.find_last_of('/'));
}
pid_t spawn(const std::string &path, const std::vector<std::string> &arguments) {
  auto pid = fork();
  if (pid < 0) throw std::runtime_error("fork failed");
  if (pid == 0) {
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(path.c_str()));
    for (const auto &argument : arguments)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    execv(path.c_str(), argv.data());
    _exit(127);
  }
  return pid;
}
void terminate(std::vector<pid_t> &children) {
  for (auto pid : children) if (pid > 0) kill(pid, SIGTERM);
  for (auto &pid : children) {
    if (pid > 0) { while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }
  }
  children.clear();
}
}
#endif
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "helix_supervisor " << helix::version << '\n';
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "Usage: helix_supervisor [--config FILE] [runtime options]\n";
      return 0;
    }
#ifdef __linux__
    auto config = helix::parse_cli(argc, argv);
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    const auto directory = executable_directory();
    std::vector<pid_t> children;
    std::uint64_t restarts = 0;
    while (!stopping) {
      children.push_back(spawn(directory + "/helix_market_data", arguments));
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      children.push_back(spawn(directory + "/helix_pricer", arguments));
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      for (int consumer = 0; consumer < config.risk_consumers; ++consumer) {
        auto risk_arguments = arguments;
        risk_arguments.insert(risk_arguments.end(), {"--consumer-id",
                                                     std::to_string(consumer)});
        children.push_back(spawn(directory + "/helix_risk", risk_arguments));
      }
      std::cout << "supervisor cohort_started restart=" << restarts
                << std::endl;
      while (!stopping) {
        bool failed = false;
        for (auto pid : children) {
          int status{};
          if (waitpid(pid, &status, WNOHANG) == pid) { failed = true; break; }
        }
        if (failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (stopping) {
        try { (void)helix::run_ctl(config, "shutdown"); } catch (...) {}
      }
      terminate(children);
      if (!stopping) { ++restarts; std::this_thread::sleep_for(std::chrono::seconds(1)); }
    }
    return 0;
#else
    std::cerr << "helix_supervisor requires Linux\n";
    return 2;
#endif
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
