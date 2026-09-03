#pragma once
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
namespace helix {
enum class ErrorDomain { configuration, numerical, ipc, protocol, lifecycle };
class Error : public std::runtime_error {
public:
  Error(ErrorDomain domain, std::string operation, std::string detail,
        int code = 0)
      : std::runtime_error(operation + ": " + detail), domain_(domain),
        operation_(std::move(operation)), code_(code) {}
  [[nodiscard]] ErrorDomain domain() const noexcept { return domain_; }
  [[nodiscard]] const std::string &operation() const noexcept { return operation_; }
  [[nodiscard]] int code() const noexcept { return code_; }
private:
  ErrorDomain domain_; std::string operation_; int code_;
};
class SystemError : public Error {
public:
  explicit SystemError(std::string operation, int code = errno)
      : Error(ErrorDomain::ipc, std::move(operation), std::strerror(code), code) {}
};
}
