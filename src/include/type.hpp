#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace qwq {

struct Type;
using TypePtr = std::shared_ptr<const Type>;

/// The type of one stream. A stream is either empty (`o`) or contains one
/// value. A pair is itself one value, so `q_q` is a single Pack type rather
/// than two adjacent top-level slots.
struct StreamType {
  TypePtr value;

  StreamType() = default;
  StreamType(TypePtr type) : value(std::move(type)) {}

  bool empty() const noexcept { return !value; }
  std::size_t size() const noexcept { return value ? 1 : 0; }
  const TypePtr& operator[](std::size_t index) const;
  const TypePtr& at(std::size_t index) const;
};

/// A first-class value type. Arrays contain one stream type per element;
/// packs contain exactly two non-empty value types. Nominal types retain their
/// class name for overload resolution and carry the runtime representation type.
/// Representation conversions are explicit via `:`; ordinary composition and
/// overload matching require exact types. Empty streams are represented by an
/// empty StreamType.
struct Type {
  enum class Kind { Int, Str, Bool, Array, Pack, Nominal, Variable };

  Kind kind;
  StreamType elements;  ///< Element stream type; only meaningful for Kind::Array
  TypePtr left;         ///< First component; only meaningful for Kind::Pack
  TypePtr right;        ///< Second component; only meaningful for Kind::Pack
  std::string name;     ///< Nominal class name; only meaningful for Kind::Nominal
  std::vector<StreamType> arguments;  ///< Nominal template arguments
  StreamType representation;  ///< Underlying stream type of a nominal class
};

TypePtr intType();
TypePtr strType();
TypePtr boolType();
TypePtr arrayType(StreamType elements);
TypePtr packType(TypePtr left, TypePtr right);
TypePtr nominalType(std::string name, std::vector<StreamType> arguments,
                    StreamType representation);
TypePtr templateVariableType(std::string name);

bool equals(const TypePtr& a, const TypePtr& b);
bool equals(const StreamType& a, const StreamType& b);

/// Combines two stream types using the strict binary pack constructor, with
/// the empty stream as the unit: o_A = A = A_o.
StreamType pack(const StreamType& left, const StreamType& right);

/// Renders q, [q], q_q, q_q_s, o, ... using left-associative pack syntax.
std::string toString(const TypePtr& type);
std::string toString(const StreamType& stream);

}  // namespace qwq
