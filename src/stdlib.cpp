#include "stdlib.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "error.hpp"

namespace qwq {
namespace {

StreamType voidStream() { return StreamType{}; }
StreamType intStream() { return StreamType(intType()); }
StreamType strStream() { return StreamType(strType()); }
StreamType boolStream() { return StreamType(boolType()); }

/// Turns one token into the value a declaration promised. The message names
/// only the type: a read is a read, whichever declaration asked for it.
Value parseToken(const std::string& token, Type::Kind kind) {
  if (kind == Type::Kind::Str) return Value::makeStr(token);

  if (kind == Type::Kind::Bool) {
    if (token == "0") return Value::makeBool(false);
    if (token == "1") return Value::makeBool(true);
    throw Error("expected a, found '" + token + "'");
  }

  std::size_t i = 0;
  bool negative = false;
  if (token[i] == '+' || token[i] == '-') {
    negative = token[i] == '-';
    ++i;
  }
  if (i == token.size()) throw Error("expected q, found '" + token + "'");

  std::uint64_t value = 0;
  for (; i < token.size(); ++i) {
    if (token[i] < '0' || token[i] > '9') {
      throw Error("expected q, found '" + token + "'");
    }
    value = value * 10 + static_cast<std::uint64_t>(token[i] - '0');
  }
  if (negative) value = 0u - value;
  return Value::makeInt(static_cast<std::int64_t>(value));
}

Value readOne(NativeIo& io, Type::Kind kind) {
  const std::optional<std::string> token = io.readToken();
  if (!token) throw Error("end of input");
  return parseToken(*token, kind);
}

Stream readInt(NativeIo& io, const Stream&) {
  return {readOne(io, Type::Kind::Int)};
}

Stream readStr(NativeIo& io, const Stream&) {
  return {readOne(io, Type::Kind::Str)};
}

Stream readBool(NativeIo& io, const Stream&) {
  return {readOne(io, Type::Kind::Bool)};
}

Stream readline(NativeIo& io, const Stream&) {
  const std::optional<std::string> line = io.readLine();
  if (!line) throw Error("end of input");
  return {Value::makeStr(*line)};
}

/// Renders a value the way the language prints it: `print : q -> o` writes the
/// digits, `print : a -> o` writes 0 or 1.
std::string textOf(const Value& value) {
  std::ostringstream text;
  text << value;
  return text.str();
}

Stream writePlain(NativeIo& io, const Stream& input) {
  std::string text;
  for (const Value& value : input) text += textOf(value);
  io.write(text);
  io.flush();
  return {};
}

Stream writeError(NativeIo& io, const Stream& input) {
  std::string text;
  for (const Value& value : input) text += textOf(value);
  io.writeError(text);
  io.flush();
  return {};
}

Stream print(NativeIo& io, const Stream& input) { return writePlain(io, input); }
Stream eprint(NativeIo& io, const Stream& input) { return writeError(io, input); }

struct Entry {
  const char* name;
  StreamType in;
  StreamType out;
  NativeFunction function;
};

/// The resolved surface of `awa/io.awa`. Overloads differ by signature, so
/// one name can print any of the three scalar types.
const std::vector<Entry>& table() {
  static const std::vector<Entry> entries = [] {
    std::vector<Entry> list;
    // One name, three signatures: `read` takes the type its context needs.
    list.push_back({"read", voidStream(), intStream(), readInt});
    list.push_back({"read", voidStream(), strStream(), readStr});
    list.push_back({"read", voidStream(), boolStream(), readBool});
    list.push_back({"readline", voidStream(), strStream(), readline});
    for (const StreamType& printed : {intStream(), strStream(), boolStream()}) {
      list.push_back({"print", printed, voidStream(), print});
      list.push_back({"eprint", printed, voidStream(), eprint});
    }
    return list;
  }();
  return entries;
}

}  // namespace

NativeFunction findNative(const std::string& name, const StreamType& in,
                          const StreamType& out) {
  for (const Entry& entry : table()) {
    if (name == entry.name && equals(entry.in, in) && equals(entry.out, out)) {
      return entry.function;
    }
  }
  return nullptr;
}

bool hasNative(const std::string& name, const StreamType& in,
               const StreamType& out) {
  return findNative(name, in, out) != nullptr;
}

}  // namespace qwq
