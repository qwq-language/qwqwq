#include "type.hpp"

namespace qwq {
namespace {

TypePtr make(Type::Kind kind, Word elements = {}) {
  auto type = std::make_shared<Type>();
  type->kind = kind;
  type->elements = std::move(elements);
  return type;
}

}  // namespace

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

TypePtr arrayType(Word elements) {
  return make(Type::Kind::Array, std::move(elements));
}

bool equals(const TypePtr& a, const TypePtr& b) {
  if (a->kind != b->kind) return false;
  if (a->kind != Type::Kind::Array) return true;
  return equals(a->elements, b->elements);
}

bool equals(const Word& a, const Word& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!equals(a[i], b[i])) return false;
  }
  return true;
}

Word concatenate(Word left, const Word& right) {
  left.insert(left.end(), right.begin(), right.end());
  return left;
}

std::string toString(const TypePtr& type) {
  switch (type->kind) {
    case Type::Kind::Int:
      return "q";
    case Type::Kind::Str:
      return "s";
    case Type::Kind::Bool:
      return "a";
    case Type::Kind::Array:
      return "[" + toString(type->elements) + "]";
  }
  return "?";
}

std::string toString(const Word& word) {
  if (word.empty()) return "o";  // the empty word

  std::string text;
  for (std::size_t i = 0; i < word.size(); ++i) {
    if (i != 0) text += "_";
    text += toString(word[i]);
  }
  return text;
}

std::string toString(const Word& word, const std::vector<std::size_t>& items) {
  if (word.empty()) return "o";

  std::size_t total = 0;
  for (const std::size_t width : items) total += width;
  if (items.empty() || total != word.size()) return toString(word);

  std::string text;
  std::size_t offset = 0;
  for (std::size_t item = 0; item < items.size(); ++item) {
    if (item != 0) text += "_";
    const std::size_t width = items[item];
    if (width == 1) {
      text += toString(word[offset]);
    } else {
      text += "(";
      for (std::size_t i = 0; i < width; ++i) {
        if (i != 0) text += "_";
        text += toString(word[offset + i]);
      }
      text += ")";
    }
    offset += width;
  }
  return text;
}

}  // namespace qwq
