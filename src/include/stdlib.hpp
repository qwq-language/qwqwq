#pragma once

#include <optional>
#include <string>
#include <vector>

#include "type.hpp"
#include "value.hpp"

namespace qwq {

/// The streams a native function reads and writes. The interpreter supplies
/// them, so the library never touches `std::cin` or `std::cout` directly.
class NativeIo {
 public:
  virtual ~NativeIo() = default;

  /// One whitespace-delimited token, or nothing at end of input.
  virtual std::optional<std::string> readToken() = 0;
  /// The rest of the current line without its newline, or nothing at end of
  /// input.
  virtual std::optional<std::string> readLine() = 0;
  virtual void write(const std::string& text) = 0;
  virtual void writeError(const std::string& text) = 0;
  virtual void flush() = 0;
};

/// A body written in C++ and declared in a `.awa` header. The input stream is
/// already checked against the declaration, so it holds either nothing or one
/// value.
using NativeFunction = Stream (*)(NativeIo& io, const Stream& input);

/// Whether the runtime implements `name` for this exact signature.
bool hasNative(const std::string& name, const StreamType& in,
               const StreamType& out);

/// The implementation, or nullptr when `hasNative` is false.
NativeFunction findNative(const std::string& name, const StreamType& in,
                          const StreamType& out);

}  // namespace qwq
