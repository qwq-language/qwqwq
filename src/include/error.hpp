#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace qwq {

/// A diagnostic raised by the lexer, parser, checker or interpreter.
///
/// `position` is a byte offset into the source text, or `npos` when the
/// diagnostic does not refer to a specific location.
class Error : public std::runtime_error {
 public:
  static constexpr std::size_t npos = std::string::npos;

  explicit Error(std::string message, std::size_t position = npos)
      : std::runtime_error(std::move(message)), position_(position) {}

  std::size_t position() const noexcept { return position_; }

 private:
  std::size_t position_;
};

}  // namespace qwq
