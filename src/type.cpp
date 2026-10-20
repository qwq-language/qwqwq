#include "type.hpp"

#include <stdexcept>
#include <utility>

namespace qwq {
namespace {

TypePtr make(Type::Kind kind, StreamType elements = {}, TypePtr left = {},
             TypePtr right = {}) {
  auto type = std::make_shared<Type>();
  type->kind = kind;
  type->elements = std::move(elements);
  type->left = std::move(left);
  type->right = std::move(right);
  return type;
}

std::string renderPackLeft(const TypePtr& type) {
  return toString(type);
}

std::string renderPackRight(const TypePtr& type) {
  const std::string text = toString(type);
  return type->kind == Type::Kind::Pack ? "(" + text + ")" : text;
}

}  // namespace

const TypePtr& StreamType::operator[](std::size_t index) const {
  return at(index);
}

const TypePtr& StreamType::at(std::size_t index) const {
  if (index != 0 || !value) throw std::out_of_range("stream type index");
  return value;
}

TypePtr intType() {
  static const TypePtr type = make(Type::Kind::Int);
  return type;
}

TypePtr strType() {
  static const TypePtr type = make(Type::Kind::Str);
  return type;
}

TypePtr boolType() {
  static const TypePtr type = make(Type::Kind::Bool);
  return type;
}

TypePtr arrayType(StreamType elements) {
  return make(Type::Kind::Array, std::move(elements));
}

TypePtr packType(TypePtr left, TypePtr right) {
  return make(Type::Kind::Pack, {}, std::move(left), std::move(right));
}

TypePtr nominalType(std::string name, std::vector<StreamType> arguments,
                    StreamType representation) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::Nominal;
  type->name = std::move(name);
  type->arguments = std::move(arguments);
  type->representation = std::move(representation);
  return type;
}

TypePtr templateVariableType(std::string name) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::Variable;
  type->name = std::move(name);
  return type;
}

bool equals(const TypePtr& a, const TypePtr& b) {
  if (a == b) return true;
  if (!a || !b || a->kind != b->kind) return false;

  switch (a->kind) {
    case Type::Kind::Int:
    case Type::Kind::Str:
    case Type::Kind::Bool:
      return true;
    case Type::Kind::Array:
      return equals(a->elements, b->elements);
    case Type::Kind::Pack:
      return equals(a->left, b->left) && equals(a->right, b->right);
    case Type::Kind::Nominal:
      if (a->name != b->name || a->arguments.size() != b->arguments.size()) {
        return false;
      }
      for (std::size_t i = 0; i < a->arguments.size(); ++i) {
        if (!equals(a->arguments[i], b->arguments[i])) return false;
      }
      return true;
    case Type::Kind::Variable:
      return a->name == b->name;
  }
  return false;
}

bool equals(const StreamType& a, const StreamType& b) {
  if (a.empty() || b.empty()) return a.empty() == b.empty();
  return equals(a.value, b.value);
}

StreamType pack(const StreamType& left, const StreamType& right) {
  if (left.empty()) return right;
  if (right.empty()) return left;
  return StreamType(packType(left.value, right.value));
}

std::string toString(const TypePtr& type) {
  if (!type) return "o";

  switch (type->kind) {
    case Type::Kind::Int:
      return "q";
    case Type::Kind::Str:
      return "s";
    case Type::Kind::Bool:
      return "a";
    case Type::Kind::Array:
      return "[" + toString(type->elements) + "]";
    case Type::Kind::Pack:
      return renderPackLeft(type->left) + "_" + renderPackRight(type->right);
    case Type::Kind::Nominal: {
      std::string result = type->name;
      if (!type->arguments.empty()) {
        result += "<";
        for (std::size_t i = 0; i < type->arguments.size(); ++i) {
          if (i != 0) result += ", ";
          result += toString(type->arguments[i]);
        }
        result += ">";
      }
      return result;
    }
    case Type::Kind::Variable:
      return type->name;
  }
  return "?";
}

std::string toString(const StreamType& stream) {
  return stream.empty() ? "o" : toString(stream.value);
}

}  // namespace qwq
