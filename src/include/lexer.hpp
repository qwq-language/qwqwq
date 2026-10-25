#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qwq {

/// A lexical token.
///
/// `_` is one connector: in a port it combines types into a binary Pack; in
/// an expression it sequentially appends morphisms. Whitespace does not change
/// its meaning. Commas separate list items. `#` is overloaded by type: it reads
/// an external value or regroups the structure of a Pack.
struct Token {
  enum class Kind {
    // eye characters (ports)
    VoidPort,   // o
    IntPort,    // q
    StrPort,    // s
    BoolPort,   // a

    // literals
    IntLiteral,
    StringLiteral,

    // punctuation
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    LeftBrace,   // { -- array literal or sized-array constructor
    RightBrace,  // }
    Comma,       // , (list separator)

    // connectors
    Underscore,  // _ (Pack in ports, sequential append in expressions)
    Compose,     // ~
    Coerce,      // :
    Loop,        // @
    Branch,      // ?

    // definitions
    Identifier,  // a function or type name
    Arrow,       // -> inside a signature
    FatArrow,    // => binds a name to a body, and names a header at line start
    Equals,      // = introduces a nominal class type

    Operator,  // + - * / % | & ^ ! == != > < >= <= $ .

    Comment,  // `// ...` to the end of the line (only when keepComments is set)
    End
  };

  Kind kind = Kind::End;
  std::string text;      ///< operator spelling, an identifier, or a decoded literal
  std::string spelling;  ///< exact source text: a string literal keeps its quotes
  std::int64_t integer = 0;  ///< value of an integer literal
  std::size_t position = 0;  ///< byte offset in the source
  bool lineBreakBefore = false;  ///< a newline occurred since the previous token
};

/// True for tokens that may start a port (`o`, `q`, `s`, `a`).
bool isPortStart(const Token& token);

/// True for tokens that may appear in the core position of a face.
bool isCoreStart(const Token& token);

/// Splits `source` into tokens.  Throws Error on invalid input.
std::vector<Token> tokenize(const std::string& source,
                            bool keepComments = false);

}  // namespace qwq
