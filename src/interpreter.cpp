#include "interpreter.hpp"

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <utility>

#include "error.hpp"

namespace qwq {
namespace {

/// Guards against runaway recursion; each call consumes C++ stack.
constexpr int kMaximumCallDepth = 2000;

/// Arithmetic follows C++: truncating division, remainder with the sign of the
/// dividend, and wraparound on overflow.  Signed overflow is undefined in C++,
/// so the wrapping operations are performed on unsigned values.
using Wide = std::uint64_t;

std::int64_t wrap(Wide value) { return static_cast<std::int64_t>(value); }

/// Splits the first `count` values off the front of `stream`.
Stream takeFront(const Stream& stream, std::size_t count) {
  return Stream(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(count));
}

/// Everything after the first `count` values.
Stream dropFront(const Stream& stream, std::size_t count) {
  return Stream(stream.begin() + static_cast<std::ptrdiff_t>(count), stream.end());
}

}  // namespace

Interpreter::Interpreter(std::ostream& output, std::istream& input,
                         std::int64_t stepLimit,
                         const std::vector<Definition>& definitions)
    : output_(output), input_(input), stepLimit_(stepLimit) {
  for (const Definition& definition : definitions) {
    if (!definition.body) continue;
    functions_[definition.name].push_back(
        Function{definition.in, definition.out, definition.body.get()});
  }
}

Stream Interpreter::run(const Expr& expr, Stream input) {
  return evaluate(expr, std::move(input));
}

Value Interpreter::readInput(const Expr& expr) {
  const TypePtr& type = expr.out.at(0);

  std::string token;
  input_ >> token;
  if (input_.fail()) {
    throw Error("'#' expected one " + toString(type) +
                    " from the input stream, but the input ended",
                expr.position);
  }

  if (type->kind == Type::Kind::Str) return Value::makeStr(token);

  if (type->kind == Type::Kind::Bool) {
    if (token == "0") return Value::makeBool(false);
    if (token == "1") return Value::makeBool(true);
    throw Error("'#' expected a (0 or 1), found '" + token + "'", expr.position);
  }

  // Int: a decimal integer.  Out-of-range values wrap, like every other
  // integer in the language.
  std::size_t i = 0;
  bool negative = false;
  if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
    negative = token[i] == '-';
    ++i;
  }
  if (i == token.size()) {
    throw Error("'#' expected a q, found '" + token + "'", expr.position);
  }

  std::uint64_t value = 0;
  for (; i < token.size(); ++i) {
    if (token[i] < '0' || token[i] > '9') {
      throw Error("'#' expected a q, found '" + token + "'", expr.position);
    }
    value = value * 10 + static_cast<std::uint64_t>(token[i] - '0');
  }
  if (negative) value = 0u - value;
  return Value::makeInt(static_cast<std::int64_t>(value));
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
          return Value::makeStr(left.text + right.text);
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
        // INT64_MIN / -1 wraps around instead of overflowing.
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
      if (left.kind == Value::Kind::Str) return Value::makeBool(left.text > right.text);
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
      if (left.kind == Value::Kind::Str) return Value::makeBool(left.text <= right.text);
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

    case Core::Duplicate: {
      Stream result = input;
      result.insert(result.end(), input.begin(), input.end());
      return result;
    }

    case Core::Read:
      return {readInput(expr)};

    case Core::Swap: {
      // Exchange the pack's two items, whatever their widths.
      const std::size_t width = expr.items.empty() ? 1 : expr.items[0];
      Stream first(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(width));
      Stream second(input.begin() + static_cast<std::ptrdiff_t>(width), input.end());
      second.insert(second.end(), std::make_move_iterator(first.begin()),
                    std::make_move_iterator(first.end()));
      return second;
    }

    case Core::ArrayLiteral: {
      // Each element is a closed literal face; evaluating it on the empty
      // stream gives the word stored for that element.
      std::vector<std::vector<Value>> words;
      words.reserve(expr.elements.size());
      for (const ExprPtr& item : expr.elements) {
        words.push_back(evaluate(*item, Stream{}));
      }
      return {Value::makeArray(Elements(std::move(words)))};
    }

    case Core::Index: {
      if (input[0].kind != Value::Kind::Array) {
        throw Error("internal error: '[]' applied to a non-array", expr.position);
      }
      const auto& elements = *input[0].elements;
      const std::int64_t index = input[1].integer;
      if (index < 0 || static_cast<std::size_t>(index) >= elements.size()) {
        throw Error("array index " + std::to_string(index) + " is out of bounds for " +
                        "an array of length " + std::to_string(elements.size()),
                    expr.position);
      }
      return elements[static_cast<std::size_t>(index)];
    }

    case Core::ArrayWrite: {
      if (input[0].kind != Value::Kind::Array) {
        throw Error("internal error: '[]' applied to a non-array", expr.position);
      }
      const std::int64_t index = input[1].integer;
      const std::size_t size = input[0].elements->size();
      if (index < 0 || static_cast<std::size_t>(index) >= size) {
        throw Error("array index " + std::to_string(index) + " is out of bounds for " +
                        "an array of length " + std::to_string(size),
                    expr.position);
      }
      const std::size_t slot = static_cast<std::size_t>(index);
      Stream value(input.begin() + 2, input.end());

      // In a queue machine the value is almost always still referenced by a
      // live stream, so unique ownership never happens in practice: writing
      // copies.  Making this O(log n) needs a persistent vector, not a check.
      return {Value::makeArray(input[0].elements->with(slot, std::move(value)))};
    }

    case Core::ArrayLength: {
      if (input[0].kind != Value::Kind::Array) {
        throw Error("internal error: '[]' applied to a non-array", expr.position);
      }
      // The element count is stored with the value, so this is O(1) and
      // copies nothing.
      return {Value::makeInt(static_cast<std::int64_t>(input[0].elements->size()))};
    }

    default:
      return {applyBinary(expr.core, input[0], input[1], expr.position)};
  }
}

Stream Interpreter::evaluateCall(const Expr& expr, Stream input) {
  const auto found = functions_.find(expr.text);
  if (found == functions_.end()) {
    throw Error("unknown function '" + expr.text + "'", expr.position);
  }

  // The checker already picked an overload; find the body with that signature.
  const Expr* body = nullptr;
  for (const Function& function : found->second) {
    if (equals(function.in, expr.in) && equals(function.out, expr.out)) {
      body = function.body;
      break;
    }
  }
  if (body == nullptr && found->second.size() == 1) body = found->second.front().body;
  if (body == nullptr) {
    throw Error("internal error: no body matches the checked signature of '" +
                    expr.text + "'",
                expr.position);
  }

  if (depth_ >= kMaximumCallDepth) {
    throw Error("maximum call depth exceeded while calling '" + expr.text +
                    "' (infinite recursion?)",
                expr.position);
  }

  ++depth_;
  Stream result = evaluate(*body, std::move(input));
  --depth_;
  return result;
}

Stream Interpreter::evaluate(const Expr& expr, Stream input) {
  tick(expr);

  if (input.size() != expr.in.size()) {
    throw Error("internal error: stream width " + std::to_string(input.size()) +
                    " does not match the expected " + std::to_string(expr.in.size()),
                expr.position);
  }

  switch (expr.kind) {
    case Expr::Kind::Wire:
      return input;

    case Expr::Kind::Face:
      return evaluateFace(expr, input);

    case Expr::Kind::Call:
      return evaluateCall(expr, input);

    case Expr::Kind::Compose: {
      Stream intermediate = evaluate(*expr.left, std::move(input));
      return evaluate(*expr.right, std::move(intermediate));
    }

    case Expr::Kind::Tee: {
      Stream intermediate = evaluate(*expr.left, std::move(input));
      for (const Value& value : intermediate) output_ << value;
      output_.flush();
      return evaluate(*expr.right, std::move(intermediate));
    }

    case Expr::Kind::Coerce: {
      Stream intermediate = evaluate(*expr.left, std::move(input));
      if (!equals(expr.left->out, expr.right->in)) {
        intermediate.clear();  // the only other coercion is T : o
      }
      return evaluate(*expr.right, std::move(intermediate));
    }

    case Expr::Kind::Append: {
      Stream left = evaluate(*expr.left, takeFront(input, expr.left->in.size()));
      Stream right = evaluate(*expr.right, dropFront(input, expr.left->in.size()));
      left.insert(left.end(), std::make_move_iterator(right.begin()),
                  std::make_move_iterator(right.end()));
      return left;
    }

    case Expr::Kind::Loop: {
      Stream current = std::move(input);
      for (;;) {
        Stream condition = evaluate(*expr.left, current);
        const bool truth = condition[0].boolean;
        Stream payload = dropFront(condition, 1);
        if (!truth) return payload;
        current = evaluate(*expr.right, std::move(payload));
      }
    }

    case Expr::Kind::Branch: {
      Stream condition = evaluate(*expr.left, std::move(input));
      const bool truth = condition[0].boolean;
      Stream payload = dropFront(condition, 1);
      if (!truth) return payload;  // if-then: the payload flows through untouched
      return evaluate(*expr.right, std::move(payload));
    }
  }

  throw Error("internal error: unknown node", expr.position);
}

}  // namespace qwq
