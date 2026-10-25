#include "interpreter.hpp"

#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "error.hpp"

namespace qwq {
namespace {

/// Arithmetic follows C++: truncating division, remainder with the sign of the
/// dividend, and wraparound on overflow. Signed overflow is undefined in C++,
/// so the wrapping operations are performed on unsigned values.
using Wide = std::uint64_t;

std::int64_t wrap(Wide value) { return static_cast<std::int64_t>(value); }

void collectReshapeValues(const Value& value, const TypePtr& type,
                          std::vector<Value>& leaves, std::size_t position) {
  // Follow structural Pack types, not runtime shape: a nominal leaf may itself
  // have a Pack representation and must remain one value here.
  if (type->kind != Type::Kind::Pack) {
    leaves.push_back(value);
    return;
  }
  if (value.kind != Value::Kind::Pack) {
    throw Error("internal error: Pack type does not match its runtime value",
                position);
  }
  collectReshapeValues(value.pack->left, type->left, leaves, position);
  collectReshapeValues(value.pack->right, type->right, leaves, position);
}

Value buildReshapedValue(const TypePtr& type, std::vector<Value>& leaves,
                         std::size_t& next, std::size_t position) {
  if (type->kind != Type::Kind::Pack) {
    if (next >= leaves.size()) {
      throw Error("internal error: not enough values while reshaping a Pack",
                  position);
    }
    return std::move(leaves[next++]);
  }
  Value left = buildReshapedValue(type->left, leaves, next, position);
  Value right = buildReshapedValue(type->right, leaves, next, position);
  return Value::makePack(std::move(left), std::move(right));
}

std::pair<Stream, Stream> splitPack(Stream stream,
                                   const StreamType& leftType,
                                   const StreamType& rightType,
                                   std::size_t position) {
  if (leftType.empty()) return {Stream{}, std::move(stream)};
  if (rightType.empty()) return {std::move(stream), Stream{}};
  if (stream.size() != 1 || stream[0].kind != Value::Kind::Pack) {
    throw Error("internal error: expected a first-class binary pack", position);
  }

  Value& packed = stream[0];
  // This pack is consumed. Move its fields when uniquely owned so a discarded
  // parent cannot keep an array shared during a later write.
  if (packed.pack.use_count() == 1) {
    Value left = std::move(packed.pack->left);
    Value right = std::move(packed.pack->right);
    return {{std::move(left)}, {std::move(right)}};
  }
  return {{packed.pack->left}, {packed.pack->right}};
}

Stream joinStreams(const Stream& left, const Stream& right,
                   std::size_t position) {
  if (left.empty()) return right;
  if (right.empty()) return left;
  if (left.size() != 1 || right.size() != 1) {
    throw Error("internal error: a stream value contains more than one slot",
                position);
  }
  return {Value::makePack(left[0], right[0])};
}

struct ConditionResult {
  bool truth;
  Stream payload;
};

ConditionResult unpackCondition(const Stream& condition, const StreamType& payloadType,
                                std::size_t position) {
  if (payloadType.empty()) {
    if (condition.size() != 1 || condition[0].kind != Value::Kind::Bool) {
      throw Error("internal error: expected a boolean condition", position);
    }
    return {condition[0].boolean, {}};
  }

  if (condition.size() != 1 || condition[0].kind != Value::Kind::Pack ||
      condition[0].pack->left.kind != Value::Kind::Bool) {
    throw Error("internal error: expected a boolean packed with the payload",
                position);
  }
  return {condition[0].pack->left.boolean,
          {condition[0].pack->right}};
}

TypePtr representationType(const TypePtr& type) {
  TypePtr current = type;
  while (current && current->kind == Type::Kind::Nominal) {
    if (current->representation.empty()) return {};
    current = current->representation.value;
  }
  return current;
}

// Default construction is structural: q -> 0, s -> "", a -> false,
// arrays -> empty arrays, and packs -> recursively defaulted components.
Value defaultValue(const TypePtr& type) {
  switch (type->kind) {
    case Type::Kind::Int:
      return Value::makeInt(0);
    case Type::Kind::Str:
      return Value::makeStr("");
    case Type::Kind::Bool:
      return Value::makeBool(false);
    case Type::Kind::Array:
      return Value::makeArray(Elements{});
    case Type::Kind::Pack:
      return Value::makePack(defaultValue(type->left), defaultValue(type->right));
    case Type::Kind::Nominal:
      if (!type->representation.empty()) {
        return defaultValue(type->representation.value);
      }
      throw Error("internal error: nominal type has no representation");
    case Type::Kind::Variable:
      throw Error("internal error: unresolved template type in runtime value");
  }
  return Value{};
}

Stream defaultStream(const StreamType& type) {
  if (type.empty()) return {};
  return {defaultValue(type.value)};
}

std::size_t checkedArrayLength(std::int64_t length, std::size_t position) {
  if (length < 0) {
    throw Error("array length must be non-negative, found " +
                    std::to_string(length),
                position);
  }

  const auto count = static_cast<std::uint64_t>(length);
  if (count > std::vector<Stream>{}.max_size()) {
    throw Error("array length exceeds the maximum supported size", position);
  }
  return static_cast<std::size_t>(count);
}

}  // namespace

Interpreter::Interpreter(std::ostream& output, std::istream& input,
                         std::int64_t stepLimit,
                         const std::vector<Definition>& definitions,
                         std::ostream* inputPrompt,
                         const std::vector<Definition>* specializations,
                         std::ostream* errors)
    : output_(output),
      input_(input),
      errors_(errors),
      inputPrompt_(inputPrompt),
      stepLimit_(stepLimit) {
  const auto add = [&](const Definition& definition) {
    if (!definition.templateParameters.empty()) return;
    if (!definition.body && !definition.external) return;
    functions_[definition.name].push_back(Function{
        definition.in, definition.out, definition.body.get(),
        definition.templateInstanceKey, definition.external});
  };
  for (const Definition& definition : definitions) add(definition);
  if (specializations != nullptr) {
    for (const Definition& definition : *specializations) add(definition);
  }
}

Stream Interpreter::run(const Expr& expr, Stream input) {
  return evaluate(expr, std::move(input));
}

/// Turns the interpreter's streams into the library's `NativeIo`. Reads are
/// counted so the REPL can tell a data line from the next command.
class Interpreter::RuntimeIo final : public NativeIo {
 public:
  RuntimeIo(std::istream& input, std::ostream& output, std::ostream& errors,
            std::size_t& reads)
      : input_(input), output_(output), errors_(errors), reads_(reads) {}

  std::optional<std::string> readToken() override {
    ++reads_;
    std::string token;
    if (!(input_ >> token)) return std::nullopt;
    return token;
  }

  std::optional<std::string> readLine() override {
    ++reads_;
    std::string line;
    if (!std::getline(input_, line)) return std::nullopt;
    return line;
  }

  void write(const std::string& text) override { output_ << text; }
  void writeError(const std::string& text) override { errors_ << text; }
  void flush() override {
    output_.flush();
    errors_.flush();
  }

 private:
  std::istream& input_;
  std::ostream& output_;
  std::ostream& errors_;
  std::size_t& reads_;
};

Stream Interpreter::callNative(NativeFunction function, const Expr& expr,
                               Stream input) {
  std::ostream& errors = errors_ != nullptr ? *errors_ : output_;
  RuntimeIo io(input_, output_, errors, inputReadCount_);
  try {
    Stream output = function(io, input);
    if (output.size() != expr.out.size()) {
      throw Error("internal error: native '" + expr.text + "' produced " +
                      std::to_string(output.size()) + " slots, expected " +
                      std::to_string(expr.out.size()),
                  expr.position);
    }
    return output;
  } catch (const Error& error) {
    if (error.position() != Error::npos) throw;
    // Native bodies report no position of their own; blame the call.
    throw Error(error.what(), expr.position);
  }
}

void Interpreter::tick(const Expr& expr) {
  if (++steps_ > stepLimit_) {
    throw Error("step limit exceeded (possible infinite loop)", expr.position);
  }
}

Value Interpreter::applyBinary(Core core, const Value& left, const Value& right,
                               std::size_t position) {
  switch (core) {
    case Core::Add:
      switch (left.kind) {
        case Value::Kind::Int:
          return Value::makeInt(wrap(Wide(left.integer) + Wide(right.integer)));
        case Value::Kind::Str:
          return Value::makeStr(*left.text + *right.text);
        case Value::Kind::Array:
          return Value::makeArray(left.elements->concatenated(*right.elements));
        default:
          break;
      }
      break;

    case Core::Sub:
      if (left.kind == Value::Kind::Int) {
        return Value::makeInt(wrap(Wide(left.integer) - Wide(right.integer)));
      }
      break;

    case Core::Mul:
      if (left.kind == Value::Kind::Int) {
        return Value::makeInt(wrap(Wide(left.integer) * Wide(right.integer)));
      }
      break;

    case Core::Div:
      if (left.kind == Value::Kind::Int) {
        if (right.integer == 0) throw Error("division by zero", position);
        if (left.integer == INT64_MIN && right.integer == -1) {
          return Value::makeInt(INT64_MIN);
        }
        return Value::makeInt(left.integer / right.integer);
      }
      break;

    case Core::Mod:
      if (left.kind == Value::Kind::Int) {
        if (right.integer == 0) throw Error("modulo by zero", position);
        if (left.integer == INT64_MIN && right.integer == -1) return Value::makeInt(0);
        return Value::makeInt(left.integer % right.integer);
      }
      break;

    case Core::BitOr:
      if (left.kind == Value::Kind::Int) return Value::makeInt(left.integer | right.integer);
      if (left.kind == Value::Kind::Bool) return Value::makeBool(left.boolean || right.boolean);
      break;

    case Core::BitAnd:
      if (left.kind == Value::Kind::Int) return Value::makeInt(left.integer & right.integer);
      if (left.kind == Value::Kind::Bool) return Value::makeBool(left.boolean && right.boolean);
      break;

    case Core::BitXor:
      if (left.kind == Value::Kind::Int) return Value::makeInt(left.integer ^ right.integer);
      if (left.kind == Value::Kind::Bool) return Value::makeBool(left.boolean != right.boolean);
      break;

    case Core::Equal:
      return Value::makeBool(left == right);

    case Core::NotEqual:
      return Value::makeBool(left != right);

    case Core::Greater:
      if (left.kind == Value::Kind::Int) return Value::makeBool(left.integer > right.integer);
      if (left.kind == Value::Kind::Str) return Value::makeBool(*left.text > *right.text);
      break;

    case Core::Less:
      if (left.kind == Value::Kind::Int) return Value::makeBool(left.integer < right.integer);
      if (left.kind == Value::Kind::Str) return Value::makeBool(left.text < right.text);
      break;

    case Core::GreaterEqual:
      if (left.kind == Value::Kind::Int) return Value::makeBool(left.integer >= right.integer);
      if (left.kind == Value::Kind::Str) return Value::makeBool(left.text >= right.text);
      break;

    case Core::LessEqual:
      if (left.kind == Value::Kind::Int) return Value::makeBool(left.integer <= right.integer);
      if (left.kind == Value::Kind::Str) return Value::makeBool(*left.text <= *right.text);
      break;

    default:
      break;
  }

  throw Error("internal error: core applied to an unsupported type", position);
}

Stream Interpreter::evaluateFace(const Expr& expr, const Stream& input) {
  switch (expr.core) {
    case Core::IntLiteral:
      return {Value::makeInt(expr.integer)};

    case Core::BoolLiteral:
      return {Value::makeBool(expr.integer != 0)};

    case Core::StringLiteral:
      return {Value::makeStr(expr.text)};

    case Core::Negate:
      return {Value::makeInt(wrap(Wide(0) - Wide(input[0].integer)))};

    case Core::Not:
      return {Value::makeBool(!input[0].boolean)};

    case Core::Duplicate:
      if (input.empty()) return {};
      return {Value::makePack(input[0], input[0])};

    case Core::Hash:
      throw Error("internal error: unresolved '#' overload", expr.position);

    case Core::Swap:
      return {Value::makePack(input[0].pack->right, input[0].pack->left)};

    case Core::Reshape: {
      if (input.size() != 1 || input[0].kind != Value::Kind::Pack ||
          expr.in.empty() || expr.out.empty()) {
        throw Error("internal error: invalid Pack reshape input", expr.position);
      }
      std::vector<Value> leaves;
      collectReshapeValues(input[0], expr.in.value, leaves, expr.position);
      std::size_t next = 0;
      Value result =
          buildReshapedValue(expr.out.value, leaves, next, expr.position);
      if (next != leaves.size()) {
        throw Error("internal error: unused values while reshaping a Pack",
                    expr.position);
      }
      return {std::move(result)};
    }

    case Core::ArrayLiteral: {
      std::vector<Stream> streams;
      streams.reserve(expr.elements.size());
      for (const ExprPtr& item : expr.elements) {
        streams.push_back(evaluate(*item, Stream{}));
      }
      return {Value::makeArray(Elements(std::move(streams)))};
    }

    case Core::ArrayFill: {
      if (input.size() != 1 || input[0].kind != Value::Kind::Pack ||
          input[0].pack->left.kind != Value::Kind::Int) {
        throw Error("internal error: invalid array-fill input", expr.position);
      }
      const std::size_t count =
          checkedArrayLength(input[0].pack->left.integer, expr.position);
      const Stream initial{input[0].pack->right};
      std::vector<Stream> streams(count, initial);
      return {Value::makeArray(Elements(std::move(streams)))};
    }

    case Core::ArrayDefault: {
      if (input.size() != 1 || input[0].kind != Value::Kind::Int) {
        throw Error("internal error: invalid default-array input", expr.position);
      }
      const std::size_t count = checkedArrayLength(input[0].integer, expr.position);
      const TypePtr outputType = representationType(expr.out.value);
      if (!outputType || outputType->kind != Type::Kind::Array) {
        throw Error("internal error: default-array output has no array representation",
                    expr.position);
      }
      const Stream initial = defaultStream(outputType->elements);
      std::vector<Stream> streams(count, initial);
      return {Value::makeArray(Elements(std::move(streams)))};
    }

    case Core::Index: {
      if (input[0].kind == Value::Kind::Array) {
        throw Error("internal error: array length was not resolved by the checker",
                    expr.position);
      }
      if (input[0].kind != Value::Kind::Pack ||
          input[0].pack->left.kind != Value::Kind::Array ||
          input[0].pack->right.kind != Value::Kind::Int) {
        throw Error("internal error: invalid array read input", expr.position);
      }
      const auto& elements = *input[0].pack->left.elements;
      const std::int64_t index = input[0].pack->right.integer;
      if (index < 0 || static_cast<std::size_t>(index) >= elements.size()) {
        throw Error("array index " + std::to_string(index) + " is out of bounds for " +
                        "an array of length " + std::to_string(elements.size()),
                    expr.position);
      }
      return elements[static_cast<std::size_t>(index)];
    }

    case Core::ArrayWrite: {
      if (input[0].kind != Value::Kind::Pack ||
          input[0].pack->left.kind != Value::Kind::Pack ||
          input[0].pack->left.pack->left.kind != Value::Kind::Array ||
          input[0].pack->left.pack->right.kind != Value::Kind::Int) {
        throw Error("internal error: invalid array write input", expr.position);
      }
      const Value& array = input[0].pack->left.pack->left;
      const std::int64_t index = input[0].pack->left.pack->right.integer;
      const std::size_t size = array.elements->size();
      if (index < 0 || static_cast<std::size_t>(index) >= size) {
        throw Error("array index " + std::to_string(index) + " is out of bounds for " +
                        "an array of length " + std::to_string(size),
                    expr.position);
      }
      const std::size_t slot = static_cast<std::size_t>(index);
      Stream value{input[0].pack->right};

      // A value copy may share either enclosing Pack or the array itself. Only
      // update the backing vector when every owner on that path is unique.
      if (input[0].pack.use_count() == 1 &&
          input[0].pack->left.pack.use_count() == 1 &&
          array.elements.use_count() == 1) {
        array.elements->replace(slot, std::move(value));
        return {array};
      }
      return {Value::makeArray(array.elements->with(slot, std::move(value)))};
    }

    case Core::ArrayLength:
      if (input[0].kind != Value::Kind::Array) {
        throw Error("internal error: '[]' applied to a non-array", expr.position);
      }
      return {Value::makeInt(static_cast<std::int64_t>(input[0].elements->size()))};

    default: {
      if (input.size() != 1 || input[0].kind != Value::Kind::Pack) {
        throw Error("internal error: binary core expected a first-class pack",
                    expr.position);
      }
      return {applyBinary(expr.core, input[0].pack->left, input[0].pack->right,
                          expr.position)};
    }
  }
}

Stream Interpreter::evaluateCall(const Expr& expr, Stream input) {
  const auto found = functions_.find(expr.text);
  if (found == functions_.end()) {
    throw Error("unknown function '" + expr.text + "'", expr.position);
  }

  // The checker already picked an overload; find its body, or the native
  // implementation a header declared.
  const Function* selected = nullptr;
  for (const Function& function : found->second) {
    if (!expr.templateInstanceKey.empty()) {
      if (function.templateInstanceKey == expr.templateInstanceKey) {
        selected = &function;
        break;
      }
    } else if (function.templateInstanceKey.empty() &&
               equals(function.in, expr.in) && equals(function.out, expr.out)) {
      selected = &function;
      break;
    }
  }
  if (selected == nullptr) {
    throw Error("internal error: no body matches the checked signature of '" +
                    expr.text + "'",
                expr.position);
  }

  if (selected->external) {
    const NativeFunction native = findNative(expr.text, expr.in, expr.out);
    if (native == nullptr) {
      throw Error("'" + expr.text +
                      "' is declared but the runtime has no implementation for " +
                      toString(expr.in) + " -> " + toString(expr.out),
                  expr.position);
    }
    return callNative(native, expr, std::move(input));
  }

  return evaluate(*selected->body, std::move(input));
}

Stream Interpreter::evaluate(const Expr& expr, Stream input) {
  tick(expr);

  if (input.size() != expr.in.size()) {
    throw Error("internal error: stream slot count " + std::to_string(input.size()) +
                    " does not match the expected " + std::to_string(expr.in.size()),
                expr.position);
  }

  switch (expr.kind) {
    case Expr::Kind::Wire:
      return input;

    case Expr::Kind::Face:
      return evaluateFace(expr, input);

    case Expr::Kind::Call:
      return evaluateCall(expr, std::move(input));

    case Expr::Kind::Compose: {
      Stream intermediate = evaluate(*expr.left, std::move(input));
      return evaluate(*expr.right, std::move(intermediate));
    }

    case Expr::Kind::Coerce: {
      Stream intermediate = evaluate(*expr.left, std::move(input));
      if (expr.right->in.empty() && !expr.left->out.empty()) intermediate.clear();
      return evaluate(*expr.right, std::move(intermediate));
    }

    case Expr::Kind::Append: {
      auto [leftInput, rightInput] =
          splitPack(std::move(input), expr.left->in, expr.right->in,
                    expr.position);
      Stream left = evaluate(*expr.left, std::move(leftInput));
      Stream right = evaluate(*expr.right, std::move(rightInput));
      return joinStreams(left, right, expr.position);
    }

    case Expr::Kind::Loop: {
      Stream current = std::move(input);
      for (;;) {
        Stream condition = evaluate(*expr.left, std::move(current));
        ConditionResult result =
            unpackCondition(condition, expr.right->in, expr.position);
        condition.clear();  // release the condition's copy before the body runs
        if (!result.truth) return std::move(result.payload);
        current = evaluate(*expr.right, std::move(result.payload));
      }
    }

    case Expr::Kind::Branch: {
      Stream condition = evaluate(*expr.left, std::move(input));
      ConditionResult result =
          unpackCondition(condition, expr.right->in, expr.position);
      condition.clear();  // release the condition's copy before the branch runs
      if (!result.truth) return std::move(result.payload);
      return evaluate(*expr.right, std::move(result.payload));
    }
  }

  throw Error("internal error: unknown node", expr.position);
}

}  // namespace qwq
