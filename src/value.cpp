#include "value.hpp"

#include <ostream>
#include <utility>

namespace qwq {

Elements::Elements(std::vector<StreamValue> streams)
    : streams_(std::move(streams)) {}

const Elements::StreamValue& Elements::operator[](std::size_t index) const {
  return streams_[index];
}

void Elements::replace(std::size_t index, StreamValue stream) {
  streams_[index] = std::move(stream);
}

Elements Elements::with(std::size_t index, StreamValue stream) const {
  Elements result = *this;
  result.replace(index, std::move(stream));
  return result;
}

Elements Elements::concatenated(const Elements& other) const {
  std::vector<StreamValue> streams = streams_;
  streams.insert(streams.end(), other.streams_.begin(), other.streams_.end());
  return Elements(std::move(streams));
}

bool Elements::operator==(const Elements& other) const {
  if (size() != other.size()) return false;
  for (std::size_t i = 0; i < size(); ++i) {
    if (streams_[i] != other.streams_[i]) return false;
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
  result.text = std::make_shared<const std::string>(std::move(value));
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
  result.elements = std::make_shared<Elements>(std::move(elements));
  return result;
}

Value Value::makePack(Value left, Value right) {
  Value result;
  result.kind = Kind::Pack;
  result.pack = std::make_shared<PackValue>(
      PackValue{std::move(left), std::move(right)});
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
      return *a.text == *b.text;
    case Value::Kind::Bool:
      return a.boolean == b.boolean;
    case Value::Kind::Array:
      return *a.elements == *b.elements;
    case Value::Kind::Pack:
      return a.pack->left == b.pack->left && a.pack->right == b.pack->right;
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, const Value& value) {
  switch (value.kind) {
    case Value::Kind::Void:
      break;
    case Value::Kind::Int:
      out << value.integer;
      break;
    case Value::Kind::Str:
      out << *value.text;
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
    case Value::Kind::Pack:
      out << "(" << value.pack->left << "_" << value.pack->right << ")";
      break;
  }
  return out;
}

}  // namespace qwq
