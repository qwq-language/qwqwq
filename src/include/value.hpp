#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace qwq {

struct Value;

/// The contents of an array.
///
/// Stored in fixed-size chunks behind shared pointers.  Copying an array is
/// therefore O(1) (one pointer) and writing one element copies a single chunk,
/// not the whole array.  Chunks are immutable once built, so every version of
/// an array can share the chunks it did not change.
class Elements {
 public:
  static constexpr std::size_t kChunkSize = 32;
  /// One element is a word: a possibly multi-item payload.
  using Chunk = std::vector<std::vector<Value>>;

  Elements() = default;
  explicit Elements(std::vector<std::vector<Value>> words);

  std::size_t size() const { return total_; }
  const std::vector<Value>& operator[](std::size_t index) const;

  /// A new version with `index` replaced by `word`; shares every other chunk.
  Elements with(std::size_t index, std::vector<Value> word) const;
  Elements concatenated(const Elements& other) const;

  bool operator==(const Elements& other) const;
  bool operator!=(const Elements& other) const { return !(*this == other); }

 private:
  std::vector<std::shared_ptr<const Chunk>> chunks_;
  std::size_t total_ = 0;
};

/// A runtime value.  `Void` carries no payload and therefore never occupies
/// a slot in the stream.
struct Value {
  enum class Kind { Void, Int, Str, Bool, Array };

  Kind kind = Kind::Void;
  std::int64_t integer = 0;
  std::string text;
  bool boolean = false;
  /// Held behind a shared pointer so that copying a value is O(1).  Only `[]`
  /// builds a new contents object, and only the chunk it wrote into is new.
  std::shared_ptr<const Elements> elements;

  static Value makeInt(std::int64_t value);
  static Value makeStr(std::string value);
  static Value makeBool(bool value);
  static Value makeArray(Elements elements);
};

bool operator==(const Value& a, const Value& b);
inline bool operator!=(const Value& a, const Value& b) { return !(a == b); }

/// Renders a value the way `;` and the final output print it.
std::ostream& operator<<(std::ostream& out, const Value& value);

/// The stream: an ordered sequence of values flowing between faces.
using Stream = std::vector<Value>;

}  // namespace qwq
