#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace qwq {

struct Value;
class Interpreter;

/// The contents of an array. Values share Elements storage and writes use
/// copy-on-write whenever the array is shared.
class Elements {
 public:
  /// One element is one stream: empty for `o`, otherwise a single value.
  using StreamValue = std::vector<Value>;

  Elements() = default;
  explicit Elements(std::vector<StreamValue> streams);

  std::size_t size() const { return streams_.size(); }
  const StreamValue& operator[](std::size_t index) const;

  /// A new version with `index` replaced by `stream`; the original is unchanged.
  Elements with(std::size_t index, StreamValue stream) const;
  Elements concatenated(const Elements& other) const;

  bool operator==(const Elements& other) const;
  bool operator!=(const Elements& other) const { return !(*this == other); }

 private:
  friend class Interpreter;

  void replace(std::size_t index, StreamValue stream);

  std::vector<StreamValue> streams_;
};

struct PackValue;

/// A runtime value. `Void` carries no payload and never occupies a slot.
/// Strings use shared immutable payloads; packs and arrays share storage and
/// use copy-on-write, so value copies stay cheap without exposing mutations.
struct Value {
  enum class Kind { Void, Int, Str, Bool, Array, Pack };

  Kind kind = Kind::Void;
  std::int64_t integer = 0;
  std::shared_ptr<const std::string> text;
  bool boolean = false;
  std::shared_ptr<Elements> elements;
  std::shared_ptr<PackValue> pack;

  static Value makeInt(std::int64_t value);
  static Value makeStr(std::string value);
  static Value makeBool(bool value);
  static Value makeArray(Elements elements);
  static Value makePack(Value left, Value right);
};

/// The two components of a first-class binary pack.
struct PackValue {
  Value left;
  Value right;
};

bool operator==(const Value& a, const Value& b);
inline bool operator!=(const Value& a, const Value& b) { return !(a == b); }

/// Renders a value the way `;` and final output print it.
std::ostream& operator<<(std::ostream& out, const Value& value);

/// One stream: either empty or exactly one value. A pack is itself a value.
using Stream = std::vector<Value>;

}  // namespace qwq
