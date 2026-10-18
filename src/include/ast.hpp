#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "type.hpp"

namespace qwq {

/// The operation core of a face: the "mouth" between the two eyes.
enum class Core {
  IntLiteral,
  StringLiteral,
  BoolLiteral,
  Negate,  // -
  Not,     // !
  Add,     // +
  Sub,     // -
  Mul,     // *
  Div,     // /
  Mod,     // %
  BitOr,   // |
  BitAnd,  // &
  BitXor,  // ^
  Equal,   // ==
  NotEqual,
  Greater,  // >
  Less,     // <
  GreaterEqual,
  LessEqual,
  Duplicate,  // $
  Swap,       // .
  Index,      // []   [T]_q -> T          read one element
  ArrayWrite, // []   [T]_q_T -> [T]      write one element
  ArrayLength,// []   [T] -> q            number of elements
  ArrayLiteral,  // {o1q o2q} : o -> [q]; each element is a literal face
  Read           // # : o -> q | s | a   one value from the input source
};

const char* name(Core core);

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Port;

/// One segment of a port.
struct PortPart {
  enum class Kind {
    Word,       ///< plain atomic types, e.g. `q` or `o`
    Array,      ///< `[T]`, a single array-valued slot
    Expression  ///< a parenthesised identity such as `(q_q)`
  };

  Kind kind = Kind::Word;
  Word word;                    ///< Kind::Word
  std::unique_ptr<Port> inner;  ///< Kind::Array: element port
  ExprPtr expression;           ///< Kind::Expression
  std::size_t position = 0;
};

/// A port is a word built from atomic types, arrays and parenthesised
/// expressions used as types.
struct Port {
  std::vector<PortPart> parts;
};

/// A node of the syntax tree.  Every node is a morphism; the checker fills
/// in `in` and `out`, so a checked tree carries its own typing.
struct Expr {
  enum class Kind {
    Wire,     ///< a bare port used as the identity morphism
    Face,     ///< `domain core codomain`
    Call,     ///< a named morphism introduced by `def`
    Compose,  ///< ~
    Tee,      ///< ;
    Coerce,   ///< :
    Append,   ///< _
    Loop,     ///< @
    Branch    ///< ?
  };

  Kind kind = Kind::Wire;

  Port domain;    ///< face domain, or the word of a Wire
  Port codomain;  ///< face codomain

  Core core = Core::Add;
  std::int64_t integer = 0;
  std::vector<ExprPtr> elements;  ///< ArrayLiteral: one closed literal face per element
  std::string text;                    ///< string literal, or the name of a Call

  ExprPtr left;
  ExprPtr right;

  Word in;   ///< filled in by the checker
  Word out;  ///< filled in by the checker

  /// Item boundaries of the two eyes, filled in by the checker.  Types ignore
  /// the grouping, but `.` is defined by it, so a pack must print as a pack.
  std::vector<std::size_t> domainItems;
  std::vector<std::size_t> codomainItems;

  /// Item granularity of a swap: the widths of the pack's two items.
  std::vector<std::size_t> items;

  std::size_t position = 0;
};

/// A named morphism:
///
///   def double : q -> q := (q $ q_q) ~ ((q_q) + q)
///
/// The declared signature is part of the definition, which lets the body
/// refer to the name being defined, i.e. recursion.  Several definitions may
/// share a name as long as their domains differ; a call is resolved by the
/// type of the stream it receives, exactly like `-` chooses between negation
/// and subtraction.
struct Definition {
  std::string name;
  Port domain;
  Port codomain;
  bool hasSignature = false;  ///< `name : A -> B => body` vs `name => body`
  ExprPtr body;
  Word in;   ///< resolved signature, filled in by the checker
  Word out;  ///< resolved signature, filled in by the checker
  std::vector<std::size_t> domainItems;    ///< item boundaries, for printing
  std::vector<std::size_t> codomainItems;  ///< item boundaries, for printing
  std::size_t position = 0;                ///< byte offset of the name
  std::size_t end = 0;                     ///< byte offset just past the body
};

/// A source file: definitions followed by an optional main expression.
struct Program {
  std::vector<Definition> definitions;
  ExprPtr main;
};

/// Renders a tree together with its inferred types, for `--dump`.
void dump(const Expr& expr, std::ostream& out, int indent = 0);
void dump(const Program& program, std::ostream& out);

}  // namespace qwq
