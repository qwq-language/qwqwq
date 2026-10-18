#include "checker.hpp"

#include <optional>
#include <string>

#include "error.hpp"

namespace qwq {
namespace {

/// A port resolved to its word plus the boundaries of its top-level items.
/// An item is one visible segment of the port: an atom, an array, or a
/// parenthesised group.  `q_q` is two items, `(q_q)` is one.
struct CheckedPort {
  Word word;
  std::vector<std::size_t> items;  ///< granularity, in slots, of each item
};

CheckedPort checkPort(Port& port, Environment& environment);
Word checkPortWord(Port& port, Environment& environment);
void checkExpr(Expr& expr, Environment& environment, const Word* hint);

// ---- helpers ------------------------------------------------------------

/// The definition whose body is currently being checked without a declared
/// signature.  A call to it there cannot be typed, so it earns its own message
/// instead of "unknown function".
const std::string* selfName = nullptr;

struct SelfScope {
  explicit SelfScope(const std::string& name) : previous_(selfName) {
    selfName = &name;
  }
  ~SelfScope() { selfName = previous_; }
  SelfScope(const SelfScope&) = delete;
  SelfScope& operator=(const SelfScope&) = delete;

 private:
  const std::string* previous_;
};

std::string describe(const Expr& expr) {
  return toString(expr.in, expr.domainItems) + " -> " +
         toString(expr.out, expr.codomainItems);
}

[[noreturn]] void fail(const Expr& expr, const std::string& message) {
  throw Error(message + " (face type: " + describe(expr) + ")", expr.position);
}

/// The only coercions are the identity and `T : o`.
bool canCoerce(const Word& from, const Word& to) {
  return equals(from, to) || to.empty();
}

bool supportsBinary(Core core, const TypePtr& type) {
  switch (core) {
    case Core::Add:
      return type->kind == Type::Kind::Int ||
             type->kind == Type::Kind::Str ||
             type->kind == Type::Kind::Array;
    case Core::Sub:
    case Core::Mul:
    case Core::Div:
    case Core::Mod:
      return type->kind == Type::Kind::Int;
    case Core::BitOr:
    case Core::BitAnd:
    case Core::BitXor:
      return type->kind == Type::Kind::Int || type->kind == Type::Kind::Bool;
    default:
      return false;
  }
}

// ---- ports --------------------------------------------------------------

CheckedPort checkPort(Port& port, Environment& environment) {
  CheckedPort checked;

  for (PortPart& part : port.parts) {
    switch (part.kind) {
      case PortPart::Kind::Word:
        checked.word.insert(checked.word.end(), part.word.begin(), part.word.end());
        if (!part.word.empty()) checked.items.push_back(part.word.size());
        break;

      case PortPart::Kind::Expression:
        checkExpr(*part.expression, environment, nullptr);
        if (!equals(part.expression->in, part.expression->out)) {
          throw Error(
              "expected a port (a type), but found the morphism " +
                  describe(*part.expression) +
                  "; the eyes of a face are types, so connect a program first: "
                  "A ~ (X . Y)",
              part.position);
        }
        // A parenthesised group is a single item, however wide it is.
        if (!part.expression->in.empty()) {
          checked.items.push_back(part.expression->in.size());
        }
        checked.word.insert(checked.word.end(), part.expression->in.begin(),
                            part.expression->in.end());
        break;

      case PortPart::Kind::Array:
        checked.word.push_back(arrayType(checkPortWord(*part.inner, environment)));
        checked.items.push_back(1);
        break;
    }
  }

  return checked;
}

Word checkPortWord(Port& port, Environment& environment) {
  return checkPort(port, environment).word;
}

/// The width of a port when it can be counted without checking anything.
std::optional<std::size_t> portWidth(const Port& port) {
  std::size_t width = 0;
  for (const PortPart& part : port.parts) {
    switch (part.kind) {
      case PortPart::Kind::Word:
        width += part.word.size();
        break;
      case PortPart::Kind::Array:
        width += 1;
        break;
      case PortPart::Kind::Expression: {
        // A parenthesised group used as a type is one item, but it still has a
        // width; without it, any face spelled with a group looked infinitely
        // wide and lost its hint.
        const Expr& inner = *part.expression;
        if (inner.kind != Expr::Kind::Wire && inner.kind != Expr::Kind::Face) {
          return std::nullopt;
        }
        const std::optional<std::size_t> innerWidth = portWidth(inner.domain);
        if (!innerWidth) return std::nullopt;
        width += *innerWidth;
        break;
      }
    }
  }
  return width;
}

/// The number of values an expression consumes, when that is already known.
/// Used to split a hinted input word across `_`, so that overloaded calls on
/// either side can still be resolved.
std::optional<std::size_t> knownWidth(const Expr& expr, Environment& environment) {
  switch (expr.kind) {
    case Expr::Kind::Wire:
    case Expr::Kind::Face:
      return portWidth(expr.domain);

    case Expr::Kind::Call: {
      const auto found = environment.find(expr.text);
      if (found == environment.end()) return std::nullopt;
      // Overloads may differ in type but agree on how many values they take.
      const std::size_t width = found->second.front().first.size();
      for (const Signature& signature : found->second) {
        if (signature.first.size() != width) return std::nullopt;
      }
      return width;
    }

    case Expr::Kind::Compose:
    case Expr::Kind::Tee:
    case Expr::Kind::Coerce:
      return knownWidth(*expr.left, environment);

    case Expr::Kind::Append: {
      const auto left = knownWidth(*expr.left, environment);
      const auto right = knownWidth(*expr.right, environment);
      if (!left || !right) return std::nullopt;
      return *left + *right;
    }

    case Expr::Kind::Loop:
    case Expr::Kind::Branch:
      return knownWidth(*expr.right, environment);
  }
  return std::nullopt;
}

bool isInt(const TypePtr& type) { return type->kind == Type::Kind::Int; }

// ---- faces --------------------------------------------------------------

void checkLiteral(Expr& expr, const Word& domain, const Word& codomain) {
  if (!domain.empty()) {
    fail(expr, "a literal does not consume input; use (T : o) to discard explicitly");
  }

  // `o0a` / `o1a`: a boolean literal, written as `o<0|1>a`.
  if (expr.core == Core::IntLiteral && codomain.size() == 1 &&
      codomain[0]->kind == Type::Kind::Bool) {
    if (expr.integer != 0 && expr.integer != 1) {
      fail(expr, "a boolean literal must be 0 or 1");
    }
    expr.core = Core::BoolLiteral;
    return;
  }

  if (expr.core == Core::IntLiteral) {
    if (codomain.size() != 1 || !isInt(codomain[0])) {
      fail(expr, "an integer literal must produce q");
    }
    return;
  }

  if (codomain.size() != 1 || codomain[0]->kind != Type::Kind::Str) {
    fail(expr, "a string literal must produce s");
  }
}

/// The elements of `{...}` are literal faces in their own right (`o1q`,
/// `o"text"s`, a nested `o{...}[q]`): each is checked as the closed morphism
/// it is, and its output word must be exactly the element word of the array.
void checkArrayLiteral(Expr& expr, const Word& domain, const Word& codomain,
                       Environment& environment) {
  if (!domain.empty()) {
    fail(expr, "an array literal does not consume input");
  }
  if (codomain.size() != 1 || codomain[0]->kind != Type::Kind::Array) {
    fail(expr, "an array literal must produce a single array");
  }
  const Word& element = codomain[0]->elements;
  const Word empty;
  for (const ExprPtr& item : expr.elements) {
    checkExpr(*item, environment, &empty);
    const bool isLiteral =
        item->kind == Expr::Kind::Face &&
        (item->core == Core::IntLiteral || item->core == Core::BoolLiteral ||
         item->core == Core::StringLiteral || item->core == Core::ArrayLiteral);
    if (!isLiteral) {
      throw Error("an element of an array literal must be a literal face, "
                  "e.g. o1q",
                  item->position);
    }
    if (!item->in.empty() || !equals(item->out, element)) {
      throw Error("an element of this array literal must have type o -> " +
                      toString(element) + ", found " + describe(*item),
                  item->position);
    }
  }
}

void checkIndex(Expr& expr, const Word& domain, const Word& codomain) {
  if (domain.empty() || domain[0]->kind != Type::Kind::Array) {
    fail(expr, "'[]' requires an array as its first value");
  }

  const Word& element = domain[0]->elements;

  if (domain.size() == 1) {
    // length: [T] -> q, the number of elements
    if (codomain.size() != 1 || !isInt(codomain[0])) {
      fail(expr, "'[]' applied to an array alone must produce a single q "
                 "(its length), found " +
                     toString(codomain));
    }
    expr.core = Core::ArrayLength;
    return;
  }

  if (domain.size() == 2) {
    // read: [T]_q -> T
    if (!isInt(domain[1])) {
      fail(expr, "'[]' expects an index of type q after the array");
    }
    if (!equals(codomain, element)) {
      fail(expr, "'[]' must produce one element of the array, i.e. " +
                     toString(element));
    }
    return;
  }

  if (domain.size() == 2 + element.size()) {
    // write: [T]_q_T -> [T]; the value flows in as the last slots
    if (!isInt(domain[1])) {
      fail(expr, "'[]' expects an index of type q after the array");
    }
    const Word value(domain.begin() + 2, domain.end());
    if (!equals(value, element)) {
      fail(expr, "'[]' writes " + toString(value) + " into an array of " +
                     toString(element));
    }
    if (codomain.size() != 1 || codomain[0]->kind != Type::Kind::Array ||
        !equals(codomain[0]->elements, element)) {
      fail(expr, "writing into " + toString(domain[0]) + " must produce " +
                     toString(domain[0]));
    }
    expr.core = Core::ArrayWrite;
    return;
  }

  fail(expr, "'[]' requires [T] for its length, [T]_q to read or [T]_q_T to "
             "write, found " +
                 toString(domain));
}

/// Fold every leading item of the port into one segment, leaving the last item
/// alone: the shortest way to turn n > 2 items into a pack of two.
std::string groupedSuggestion(const CheckedPort& port) {
  const std::size_t last = port.items.back();
  const std::size_t head = port.word.size() - last;
  return toString(port.word, {head, last});
}

/// Split a single wide item into two segments of roughly equal width.
std::string splitSuggestion(const CheckedPort& port) {
  const std::size_t width = port.word.size();
  return toString(port.word, {width / 2, width - width / 2});
}

/// `.` is only defined on a pack of exactly two items, and both eyes state the
/// same pack, so each eye must name two items.
[[noreturn]] void failPack(const Expr& expr, const CheckedPort& port,
                           const char* which) {
  const std::string rendered = toString(port.word, port.items);

  if (port.items.size() == 1 && port.word.size() >= 2) {
    fail(expr, std::string("'.' needs a pack of exactly two items, but the ") +
                   which + " " + rendered + " is one item of width " +
                   std::to_string(port.word.size()) +
                   "; split it into two segments, e.g. " + splitSuggestion(port));
  }
  if (port.items.size() > 2) {
    fail(expr, std::string("'.' needs a pack of exactly two items, but the ") +
                   which + " " + rendered + " has " +
                   std::to_string(port.items.size()) +
                   " items; group the leading ones into one segment, e.g. " +
                   groupedSuggestion(port));
  }
  fail(expr, std::string("'.' needs a pack of exactly two items, but the ") +
                 which + " " + rendered +
                 " is a single atom; a pack is two items, e.g. q_q");
}

void checkFace(Expr& expr, const CheckedPort& domainPort,
               const CheckedPort& codomainPort, Environment& environment) {
  const Word& domain = domainPort.word;
  const Word& codomain = codomainPort.word;

  // `-` is one core with two arities; the domain decides which.
  //   q - q      negate : q -> q
  //   (q_q) - q  subtract : q_q -> q
  if (expr.core == Core::Negate && domain.size() == 2) expr.core = Core::Sub;

  switch (expr.core) {
    case Core::IntLiteral:
    case Core::StringLiteral:
      checkLiteral(expr, domain, codomain);
      return;

    case Core::BoolLiteral:
      fail(expr, "internal error: unnormalised boolean literal");

    case Core::ArrayLiteral:
      checkArrayLiteral(expr, domain, codomain, environment);
      return;

    case Core::Read:
      // `o#q` is a source, exactly like `o5q`: it consumes nothing from the
      // stream and pulls one value from the input source instead.
      if (!domain.empty()) {
        fail(expr, "'#' reads from the input source, so it consumes nothing; "
                   "its domain must be o");
      }
      if (codomain.size() != 1) {
        fail(expr, "'#' reads exactly one value, so its codomain must be a "
                   "single q, s or a");
      }
      if (codomain[0]->kind == Type::Kind::Array) {
        fail(expr, "'#' cannot read " + toString(codomain[0]) +
                       ": an array has no input format; read its elements one "
                       "at a time");
      }
      return;

    case Core::Index:
      checkIndex(expr, domain, codomain);
      return;

    case Core::Negate:
      if (domain.size() != 1 || !isInt(domain[0])) {
        fail(expr, "unary '-' requires a domain of q");
      }
      if (codomain.size() != 1 || !isInt(codomain[0])) {
        fail(expr, "unary '-' requires a codomain of q");
      }
      return;

    case Core::Not:
      if (domain.size() != 1 || domain[0]->kind != Type::Kind::Bool) {
        fail(expr, "unary '!' requires a domain of a");
      }
      if (codomain.size() != 1 || codomain[0]->kind != Type::Kind::Bool) {
        fail(expr, "unary '!' requires a codomain of a");
      }
      return;

    case Core::Duplicate:
      // `$` turns the word x into two items, each of them x.  The codomain is
      // therefore a pack of two items of equal width, so `q_q $ q_q_q_q` is
      // four one-slot items and does not say "two copies of q_q".
      if (!equals(codomain, concatenate(domain, domain)) ||
          codomainPort.items.size() != 2 ||
          codomainPort.items[0] != domain.size() ||
          codomainPort.items[1] != domain.size()) {
        fail(expr, "$ requires a codomain of (x)_(x) for a domain of x");
      }
      return;

    case Core::Swap: {
      // `.` exchanges the two items of a pack, and a pack always has exactly
      // two items.  Both eyes describe that same pack, so both must state two
      // items.  An item is one segment of the port — an atom, an array, or a
      // parenthesised group — and may be as wide as it likes.
      if (domainPort.items.size() != 2) failPack(expr, domainPort, "domain");
      if (codomainPort.items.size() != 2) failPack(expr, codomainPort, "codomain");

      const std::size_t width = domainPort.items[0];
      const Word first(domain.begin(),
                       domain.begin() + static_cast<std::ptrdiff_t>(width));
      const Word second(domain.begin() + static_cast<std::ptrdiff_t>(width),
                        domain.end());
      const Word exchanged = concatenate(second, first);

      if (!equals(codomain, exchanged)) {
        fail(expr, "'.' must exchange the two items of the pack, producing " +
                       toString(exchanged,
                                {domainPort.items[1], domainPort.items[0]}));
      }

      expr.items = domainPort.items;
      return;
    }

    case Core::Equal:
    case Core::NotEqual:
      if (domain.size() != 2 || !equals(domain[0], domain[1])) {
        fail(expr, "equality requires a domain of two values of the same type");
      }
      if (codomain.size() != 1 || codomain[0]->kind != Type::Kind::Bool) {
        fail(expr, "equality must produce a");
      }
      return;

    case Core::Greater:
    case Core::Less:
    case Core::GreaterEqual:
    case Core::LessEqual:
      if (domain.size() != 2 || !equals(domain[0], domain[1])) {
        fail(expr, "ordering requires a domain of two values of the same type");
      }
      if (domain[0]->kind != Type::Kind::Int && domain[0]->kind != Type::Kind::Str) {
        fail(expr, "ordering is defined for q and s only");
      }
      if (codomain.size() != 1 || codomain[0]->kind != Type::Kind::Bool) {
        fail(expr, "ordering must produce a");
      }
      return;

    default:  // arithmetic, bitwise and logical binary cores
      if (domain.size() != 2 || !equals(domain[0], domain[1])) {
        fail(expr, "a binary core requires a domain of two values of the same type, "
                   "e.g. (q_q)");
      }
      if (!supportsBinary(expr.core, domain[0])) {
        fail(expr, std::string("core '") + name(expr.core) + "' is not defined for " +
                       toString(domain[0]));
      }
      if (codomain.size() != 1 || !equals(codomain[0], domain[0])) {
        fail(expr, "a binary core must preserve its operand type");
      }
      return;
  }
}

// ---- calls and overloading ----------------------------------------------

std::string describeOverloads(const std::string& name, const Overloads& overloads) {
  std::string text = "candidates are:";
  for (const Signature& signature : overloads) {
    text += " " + name + " : " + toString(signature.first) + " -> " +
            toString(signature.second) + ";";
  }
  if (!overloads.empty()) text.pop_back();
  return text;
}

void checkCall(Expr& expr, Environment& environment, const Word* hint) {
  const auto found = environment.find(expr.text);
  if (found == environment.end()) {
    if (selfName != nullptr && expr.text == *selfName) {
      throw Error("'" + expr.text +
                      "' cannot be called in its own body: without a signature its "
                      "type is not known yet; write `" + expr.text +
                      " : A -> B => body`",
                  expr.position);
    }
    throw Error("unknown function '" + expr.text + "'", expr.position);
  }

  const Overloads& overloads = found->second;
  const Signature* chosen = nullptr;

  if (hint != nullptr) {
    for (const Signature& signature : overloads) {
      if (equals(signature.first, *hint)) chosen = &signature;
    }
  } else if (overloads.size() == 1) {
    chosen = &overloads.front();
  }

  if (chosen == nullptr) {
    if (hint != nullptr) {
      throw Error("no overload of '" + expr.text + "' accepts " + toString(*hint) +
                      "; " + describeOverloads(expr.text, overloads),
                  expr.position);
    }
    throw Error("ambiguous call to '" + expr.text +
                    "': the incoming stream type cannot be inferred here; " +
                    describeOverloads(expr.text, overloads) +
                    ".  Insert an explicit wire, e.g. q_q ~ " + expr.text,
                expr.position);
  }

  expr.in = chosen->first;
  expr.out = chosen->second;
}

// ---- connectors ---------------------------------------------------------

void checkCompose(Expr& expr, Environment& environment, const Word* hint,
                  const char* symbol) {
  checkExpr(*expr.left, environment, hint);
  checkExpr(*expr.right, environment, &expr.left->out);

  if (!equals(expr.left->out, expr.right->in)) {
    throw Error(std::string("type mismatch at '") + symbol + "': " +
                    toString(expr.left->out) + " cannot connect to " +
                    toString(expr.right->in),
                expr.position);
  }

  expr.in = expr.left->in;
  expr.out = expr.right->out;
  expr.domainItems = expr.left->domainItems;
  expr.codomainItems = expr.right->codomainItems;
}

void checkCoerce(Expr& expr, Environment& environment, const Word* hint) {
  checkExpr(*expr.left, environment, hint);
  checkExpr(*expr.right, environment, &expr.left->out);

  if (!canCoerce(expr.left->out, expr.right->in)) {
    throw Error("no coercion from " + toString(expr.left->out) + " to " +
                    toString(expr.right->in) + "; only the identity and T : o are defined",
                expr.position);
  }

  expr.in = expr.left->in;
  expr.out = expr.right->in;
  expr.domainItems = expr.left->domainItems;
  expr.codomainItems = expr.right->domainItems;
}

void checkAppend(Expr& expr, Environment& environment, const Word* hint) {
  std::optional<Word> leftHint;
  std::optional<Word> rightHint;

  if (hint != nullptr) {
    const std::optional<std::size_t> leftWidth = knownWidth(*expr.left, environment);

    if (leftWidth && *leftWidth <= hint->size()) {
      const std::size_t cut = *leftWidth;
      leftHint = Word(hint->begin(), hint->begin() + static_cast<std::ptrdiff_t>(cut));
      rightHint = Word(hint->begin() + static_cast<std::ptrdiff_t>(cut), hint->end());
    } else if (const auto rightWidth = knownWidth(*expr.right, environment);
               rightWidth && *rightWidth <= hint->size()) {
      const std::size_t cut = hint->size() - *rightWidth;
      leftHint = Word(hint->begin(), hint->begin() + static_cast<std::ptrdiff_t>(cut));
      rightHint = Word(hint->begin() + static_cast<std::ptrdiff_t>(cut), hint->end());
    }
  }

  checkExpr(*expr.left, environment, leftHint ? &*leftHint : nullptr);
  checkExpr(*expr.right, environment, rightHint ? &*rightHint : nullptr);

  expr.in = concatenate(expr.left->in, expr.right->in);
  expr.out = concatenate(expr.left->out, expr.right->out);
  expr.domainItems = expr.left->domainItems;
  expr.domainItems.insert(expr.domainItems.end(), expr.right->domainItems.begin(),
                          expr.right->domainItems.end());
  expr.codomainItems = expr.left->codomainItems;
  expr.codomainItems.insert(expr.codomainItems.end(),
                            expr.right->codomainItems.begin(),
                            expr.right->codomainItems.end());
}

void checkControl(Expr& expr, Environment& environment, const Word* hint) {
  checkExpr(*expr.left, environment, hint);
  checkExpr(*expr.right, environment, hint);

  const bool isLoop = expr.kind == Expr::Kind::Loop;
  const std::string label = isLoop ? "@ (loop)" : "? (branch)";

  // The body must preserve the stream, so that both outcomes agree on a type.
  if (!equals(expr.right->in, expr.right->out)) {
    throw Error(label + " body must be an endomorphism X -> X, found " +
                    describe(*expr.right),
                expr.right->position);
  }

  const Word& payload = expr.right->in;

  if (!equals(expr.left->in, payload)) {
    throw Error(label + " condition must accept " + toString(payload) + ", found " +
                    toString(expr.left->in),
                expr.left->position);
  }

  const Word expected = concatenate(Word{boolType()}, payload);
  if (!equals(expr.left->out, expected)) {
    throw Error(label + " condition must produce " + toString(expected) +
                    " (a boolean followed by the payload), found " +
                    toString(expr.left->out),
                expr.left->position);
  }

  expr.in = payload;
  expr.out = payload;
  expr.domainItems = expr.right->domainItems;
  expr.codomainItems = expr.right->codomainItems;
}

// ---- entry --------------------------------------------------------------

void checkExpr(Expr& expr, Environment& environment, const Word* hint) {
  switch (expr.kind) {
    case Expr::Kind::Wire: {
      const CheckedPort port = checkPort(expr.domain, environment);
      expr.in = port.word;
      expr.out = port.word;
      expr.domainItems = port.items;
      expr.codomainItems = port.items;
      return;
    }

    case Expr::Kind::Face: {
      const CheckedPort domain = checkPort(expr.domain, environment);
      const CheckedPort codomain = checkPort(expr.codomain, environment);
      expr.in = domain.word;
      expr.out = codomain.word;
      expr.domainItems = domain.items;
      expr.codomainItems = codomain.items;
      checkFace(expr, domain, codomain, environment);
      return;
    }

    case Expr::Kind::Call:
      checkCall(expr, environment, hint);
      return;

    case Expr::Kind::Compose:
      checkCompose(expr, environment, hint, "~");
      return;

    case Expr::Kind::Tee:
      checkCompose(expr, environment, hint, ";");
      return;

    case Expr::Kind::Coerce:
      checkCoerce(expr, environment, hint);
      return;

    case Expr::Kind::Append:
      checkAppend(expr, environment, hint);
      return;

    case Expr::Kind::Loop:
    case Expr::Kind::Branch:
      checkControl(expr, environment, hint);
      return;
  }
}

}  // namespace

void check(Expr& expression, const Environment& environment) {
  Environment copy = environment;
  const Word empty;
  checkExpr(expression, copy, &empty);
}

void check(Program& program) {
  Environment environment;

  for (Definition& definition : program.definitions) {
    if (!definition.body) {
      throw Error("function '" + definition.name + "' has no body",
                  definition.position);
    }

    if (!definition.hasSignature) {
      // `name => body`: the body is checked first and its two eyes become the
      // signature.  The name is therefore invisible while the body is checked,
      // which is exactly why recursion has to state a signature.
      const SelfScope scope(definition.name);
      checkExpr(*definition.body, environment, nullptr);

      definition.in = definition.body->in;
      definition.out = definition.body->out;
      definition.domainItems = definition.body->domainItems;
      definition.codomainItems = definition.body->codomainItems;

      Overloads& overloads = environment[definition.name];
      for (const Signature& signature : overloads) {
        if (equals(signature.first, definition.in)) {
          throw Error("'" + definition.name + "' is already defined for " +
                          toString(definition.in, definition.domainItems) +
                          "; overloads must differ in their domain",
                      definition.position);
        }
      }
      overloads.emplace_back(definition.in, definition.out);
      continue;
    }

    const CheckedPort declaredDomain = checkPort(definition.domain, environment);
    const CheckedPort declaredCodomain = checkPort(definition.codomain, environment);
    definition.in = declaredDomain.word;
    definition.out = declaredCodomain.word;
    definition.domainItems = declaredDomain.items;
    definition.codomainItems = declaredCodomain.items;

    Overloads& overloads = environment[definition.name];
    for (const Signature& signature : overloads) {
      if (equals(signature.first, definition.in)) {
        throw Error("'" + definition.name + "' is already defined for " +
                        toString(definition.in, definition.domainItems) +
                        "; overloads must differ in their domain",
                    definition.position);
      }
    }

    // Bind the name before checking the body, so the body may call itself.
    overloads.emplace_back(definition.in, definition.out);

    checkExpr(*definition.body, environment, &definition.in);

    if (!equals(definition.body->in, definition.in) ||
        !equals(definition.body->out, definition.out)) {
      throw Error("function '" + definition.name + "' is declared as " +
                      toString(definition.in, definition.domainItems) + " -> " +
                      toString(definition.out, definition.codomainItems) +
                      " but its body is " + describe(*definition.body),
                  definition.body->position);
    }
  }

  if (program.main) {
    const Word empty;  // a program starts from the empty stream
    checkExpr(*program.main, environment, &empty);
  }
}

}  // namespace qwq
