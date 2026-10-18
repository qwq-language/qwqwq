#include "value.hpp"

#include <algorithm>
#include <iterator>
#include <ostream>

namespace qwq {

Elements::Elements(std::vector<std::vector<Value>> words) : total_(words.size()) {
  chunks_.reserve((words.size() + kChunkSize - 1) / kChunkSize);
  for (std::size_t begin = 0; begin < words.size(); begin += kChunkSize) {
    const std::size_t end = std::min(begin + kChunkSize, words.size());
    Chunk chunk(std::make_move_iterator(words.begin() + static_cast<std::ptrdiff_t>(begin)),
                std::make_move_iterator(words.begin() + static_cast<std::ptrdiff_t>(end)));
    chunks_.push_back(std::make_shared<const Chunk>(std::move(chunk)));
  }
}

const std::vector<Value>& Elements::operator[](std::size_t index) const {
  return (*chunks_[index / kChunkSize])[index % kChunkSize];
}

Elements Elements::with(std::size_t index, std::vector<Value> word) const {
  Elements result = *this;
  const std::size_t which = index / kChunkSize;
  auto chunk = std::make_shared<Chunk>(*result.chunks_[which]);
  (*chunk)[index % kChunkSize] = std::move(word);
  result.chunks_[which] = std::move(chunk);
  return result;
}

Elements Elements::concatenated(const Elements& other) const {
  std::vector<std::vector<Value>> words;
  words.reserve(size() + other.size());
  for (std::size_t i = 0; i < size(); ++i) words.push_back((*this)[i]);
  for (std::size_t i = 0; i < other.size(); ++i) words.push_back(other[i]);
  return Elements(std::move(words));
}

bool Elements::operator==(const Elements& other) const {
  if (size() != other.size()) return false;
  for (std::size_t i = 0; i < size(); ++i) {
    if ((*this)[i] != other[i]) return false;
  }
  return true;
}

Value Value::makeInt(std::int64_t value) {
  Value result;
  result.kind = Kind::Int;
  result.integer = value;
  return result;
}

Value Value::makeStr(std::string value) {
  Value result;
  result.kind = Kind::Str;
  result.text = std::move(value);
  return result;
}

Value Value::makeBool(bool value) {
  Value result;
  result.kind = Kind::Bool;
  result.boolean = value;
  return result;
}

Value Value::makeArray(Elements elements) {
  Value result;
  result.kind = Kind::Array;
  result.elements = std::make_shared<const Elements>(std::move(elements));
  return result;
}

bool operator==(const Value& a, const Value& b) {
  if (a.kind != b.kind) return false;

  switch (a.kind) {
    case Value::Kind::Void:
      return true;
    case Value::Kind::Int:
      return a.integer == b.integer;
    case Value::Kind::Str:
      return a.text == b.text;
    case Value::Kind::Bool:
      return a.boolean == b.boolean;
    case Value::Kind::Array:
      return *a.elements == *b.elements;
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, const Value& value) {
  switch (value.kind) {
    case Value::Kind::Void:
      break;  // no payload, nothing to print
    case Value::Kind::Int:
      out << value.integer;
      break;
    case Value::Kind::Str:
      out << value.text;  // escapes were resolved by the lexer
      break;
    case Value::Kind::Bool:
      out << (value.boolean ? 1 : 0);
      break;
    case Value::Kind::Array:
      out << "[";
      for (std::size_t i = 0; i < value.elements->size(); ++i) {
        if (i != 0) out << " ";
        for (const Value& element : (*value.elements)[i]) out << element;
      }
      out << "]";
      break;
  }
  return out;
}

}  // namespace qwq
