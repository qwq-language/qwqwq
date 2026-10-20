#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "type.hpp"

namespace qwq {

/// The operation core.
enum class Core {
  IntLiteral,
  StringLiteral,
  BoolLiteral,
  Negate,       // -
  Not,          // !
  Add,          // +
  Sub,          // -
  Mul,          // *
  Div,          // /
  Mod,          // %
  BitOr,        // |
  BitAnd,       // &
  BitXor,       // ^
  Equal,        // ==
  NotEqual,     // !=
  Greater,      // >
  Less,         // <
  GreaterEqual, // >=
  LessEqual,    // <=
  Duplicate,    // $
  Swap,         // .
  Hash,         // #            parser marker for Read or Pack reshaping
  Reshape,      // #            regroup Pack structure without reordering leaves
  Index,        // []           [T]_q -> T         read one element
  ArrayWrite,   // []           [T]_q_T -> [T]     write one element
  ArrayLength,  // []           [T] -> q           number of elements
  ArrayLiteral, // { ... }      o -> [T]           each element is a literal face
  ArrayFill,    // {}           q_T -> [T]         fill with the input value
  ArrayDefault, // {}           q -> [T]           fill with default values
  Read          // #            o -> T             one value from the input source
};

const char* name(Core core);

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Port;

/// One part of a port expression.
struct PortPart {
  enum class Kind {
    Primitive,  ///< a primitive type, e.g. `q` or the unit `o`
    Array,      ///< `[T]`, a single array-valued value
    Group,      ///< `(T)`, a grouped type that remains one Pack value
    Named,      ///< a nominal class type or template application
    Expression  ///< a parenthesised identity used as a grouped type
  };

  Kind kind = Kind::Primitive;
  StreamType type;               ///< Kind::Primitive
  std::unique_ptr<Port> inner;   ///< Kind::Array or Kind::Group
  std::string name;              ///< Kind::Named
  std::vector<std::unique_ptr<Port>> arguments;  ///< Kind::Named
  ExprPtr expression;            ///< Kind::Expression
  std::size_t position = 0;
};

/// A port denotes one stream type. Underscores combine parts into nested,
/// left-associative binary Packs; parentheses control grouping.
struct Port {
  std::vector<PortPart> parts;
};

/// A node of the syntax tree.  Every node is a morphism; the checker fills
/// in `in` and `out`, so a checked tree carries its own typing.
struct Expr {
  enum class Kind {
    Wire,     ///< a bare port used as the identity morphism
    Face,     ///< optional `domain core codomain` with bidirectional inference
    Call,     ///< a named morphism with optional input/output annotations
    Compose,  ///< explicit `~` or implicit composition by juxtaposition
    Tee,      ///< ;
    Coerce,   ///< : (explicit representation conversion or stream discard)
    Append,   ///< _ (sequential stream append; same token as port Pack)
    Loop,     ///< @
    Branch    ///< ?
  };

  Kind kind = Kind::Wire;

  Port domain;    ///< face/call domain, or the stream type of a Wire
  Port codomain;  ///< face/call codomain
  bool hasDomain = false;    ///< whether a face/call writes its input type
  bool hasCodomain = false;  ///< whether a face/call writes its output type

  Core core = Core::Add;
  std::size_t corePosition = 0;  ///< source position of the face's core token
  std::int64_t integer = 0;
  std::vector<ExprPtr> elements;  ///< ArrayLiteral: one closed literal expression per element
  std::string text;                    ///< string literal, or the name of a Call
  std::vector<Port> templateArguments;  ///< explicit function-template arguments
  std::string templateInstanceKey;     ///< selected monomorphized template body

  ExprPtr left;
  ExprPtr right;

  StreamType in;   ///< filled in by the checker
  StreamType out;  ///< filled in by the checker
  bool implicitComposition = false;  ///< adjacent stages without a written `~`
  bool parenthesized = false;        ///< the expression was explicitly grouped

  std::size_t position = 0;
};

/// A nominal class type. Its body describes the runtime representation;
/// template parameters stand for complete stream types. Runtime values use the
/// representation directly, while overload resolution retains the class name.
struct TypeAliasDefinition {
  std::string name;
  std::vector<std::string> parameters;
  Port body;
  std::size_t position = 0;
  std::size_t end = 0;
};

/// A named morphism. The declared signature is part of the definition, which
/// lets the body refer to the name being defined, i.e. recursion. Several
/// definitions may share a name as long as their (input, output) signatures
/// differ; a call is resolved from either side when possible.
struct Definition {
  std::string name;
  std::vector<std::string> templateParameters;
  Port domain;
  Port codomain;
  bool hasSignature = false;  ///< `name : A -> B => body` vs `name => body`
  ExprPtr body;
  StreamType in;   ///< resolved signature, filled in by the checker
  StreamType out;  ///< resolved signature, filled in by the checker
  std::string templateInstanceKey;  ///< key for a checked template specialization
  std::size_t position = 0;                ///< byte offset of the name
  std::size_t end = 0;                     ///< byte offset just past the body
};

/// A source file: nominal class type declarations, function definitions, and
/// an optional main expression.
struct Program {
  std::vector<TypeAliasDefinition> typeAliases;
  std::vector<Definition> definitions;
  std::vector<Definition> specializations;
  ExprPtr main;
};

/// Renders the syntax of a structural port type, including unresolved template
/// parameters. Used by REPL definition listings and program dumps.
std::string portSource(const Port& port);

/// Renders a tree together with its inferred types, for `--dump`.
void dump(const Expr& expr, std::ostream& out, int indent = 0);
void dump(const Program& program, std::ostream& out);

}  // namespace qwq
