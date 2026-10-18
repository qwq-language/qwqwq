#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qwq {

/// A lexical token.
///
/// Two spellings of `_` are distinguished by surrounding whitespace:
///
///   q_q     Join    concatenation inside a word
///   q _ q   Append  a stream connector between two faces
///
/// This keeps `q _ o0q` (a face on the right) unambiguous while allowing
/// `q_q` to mean a single two-slot word.
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
    LeftBrace,   // { -- array literal
    RightBrace,  // }
    Join,    // `_` without surrounding blanks
    Append,  // `_` with a blank on either side

    // connectors
    Compose,  // ~
    Tee,      // ;
    Coerce,   // :
    Loop,     // @
    Branch,   // ?

    // definitions
    Identifier,  // a function name
    Arrow,       // -> inside a signature
    FatArrow,    // => binds a name to a body

    Operator,  // + - * / % | & ^ ! == != > < >= <= $ .

    Comment,  // `// ...` to the end of the line (only when keepComments is set)
    End
  };

  Kind kind = Kind::End;
  std::string text;      ///< operator spelling, an identifier, or a decoded literal
  std::string spelling;  ///< exact source text: a string literal keeps its quotes
  std::int64_t integer = 0;  ///< value of an integer literal
  std::size_t position = 0;  ///< byte offset in the source
};

/// True for tokens that may start a port (`o`, `q`, `s`, `a`).
bool isPortStart(const Token& token);

/// True for tokens that may appear in the core position of a face.
bool isCoreStart(const Token& token);

/// Splits `source` into tokens.  Throws Error on invalid input.
std::vector<Token> tokenize(const std::string& source,
                            bool keepComments = false);

}  // namespace qwq
