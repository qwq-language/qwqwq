#include "lexer.hpp"

#include <cctype>

#include "error.hpp"

namespace qwq {
namespace {

bool isBlank(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

bool isLetter(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }

bool isOperatorCharacter(char c) {
  const std::string operators = "+-*/%|&^!<>$.";
  return operators.find(c) != std::string::npos;
}

Token makeToken(Token::Kind kind, std::size_t position) {
  Token token;
  token.kind = kind;
  token.position = position;
  return token;
}

Token makeText(Token::Kind kind, std::string text, std::size_t position) {
  Token token = makeToken(kind, position);
  token.spelling = text;
  token.text = std::move(text);
  return token;
}

/// Classifies a run of letters: an eye character or a name.
Token classifyWord(const std::string& word, std::size_t position) {
  if (word.size() == 1) {
    switch (word[0]) {
      case 'o': return makeText(Token::Kind::VoidPort, word, position);
      case 'q': return makeText(Token::Kind::IntPort, word, position);
      case 's': return makeText(Token::Kind::StrPort, word, position);
      case 'a': return makeText(Token::Kind::BoolPort, word, position);
      default: break;
    }
  }
  return makeText(Token::Kind::Identifier, word, position);
}

}  // namespace

bool isPortStart(const Token& token) {
  switch (token.kind) {
    case Token::Kind::VoidPort:
    case Token::Kind::IntPort:
    case Token::Kind::StrPort:
    case Token::Kind::BoolPort:
      return true;
    default:
      return false;
  }
}

bool isCoreStart(const Token& token) {
  switch (token.kind) {
    case Token::Kind::IntLiteral:
    case Token::Kind::StringLiteral:
    case Token::Kind::Operator:
    case Token::Kind::LeftBrace:  // an array literal is a core
      return true;
    default:
      return false;
  }
}

std::vector<Token> tokenize(const std::string& source, bool keepComments) {
  std::vector<Token> tokens;
  std::size_t i = 0;

  while (i < source.size()) {
    const char c = source[i];

    if (isBlank(c)) {
      ++i;
      continue;
    }

    // Comment extension: '//' to end of line.
    if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      const std::size_t begin = i;
      while (i < source.size() && source[i] != '\n') ++i;
      if (keepComments) {
        tokens.push_back(makeText(Token::Kind::Comment,
                                  source.substr(begin, i - begin), begin));
      }
      continue;
    }

    // Words: a single eye character or an identifier (function/type name).
    // Names are letters only, so `Point7` still lexes as Point, 7.
    if (isLetter(c)) {
      const std::size_t begin = i;
      while (i < source.size() && isLetter(source[i])) ++i;
      tokens.push_back(classifyWord(source.substr(begin, i - begin), begin));
      continue;
    }

    switch (c) {
      case 'o':
      case 'q':
      case 's':
      case 'a':
        break;  // unreachable: handled above
      case '(': tokens.push_back(makeText(Token::Kind::LeftParen, "(", i++)); continue;
      case ')': tokens.push_back(makeText(Token::Kind::RightParen, ")", i++)); continue;
      case '[': tokens.push_back(makeText(Token::Kind::LeftBracket, "[", i++)); continue;
      case ']': tokens.push_back(makeText(Token::Kind::RightBracket, "]", i++)); continue;
      case '{': tokens.push_back(makeText(Token::Kind::LeftBrace, "{", i++)); continue;
      case '}': tokens.push_back(makeText(Token::Kind::RightBrace, "}", i++)); continue;
      case ',': tokens.push_back(makeText(Token::Kind::Comma, ",", i++)); continue;
      case '_':
        tokens.push_back(makeText(Token::Kind::Underscore, "_", i++));
        continue;
      case '~': tokens.push_back(makeText(Token::Kind::Compose, "~", i++)); continue;
      case ';': tokens.push_back(makeText(Token::Kind::Tee, ";", i++)); continue;
      case ':': tokens.push_back(makeText(Token::Kind::Coerce, ":", i++)); continue;
      case '@': tokens.push_back(makeText(Token::Kind::Loop, "@", i++)); continue;
      case '?': tokens.push_back(makeText(Token::Kind::Branch, "?", i++)); continue;
      case '.': tokens.push_back(makeText(Token::Kind::Operator, ".", i++)); continue;
      case '#': tokens.push_back(makeText(Token::Kind::Operator, "#", i++)); continue;
      default: break;
    }

    if (isDigit(c)) {
      const std::size_t begin = i;
      // Accumulate unsigned: a literal out of range wraps instead of
      // overflowing, matching the arithmetic of the language.
      std::uint64_t value = 0;
      while (i < source.size() && isDigit(source[i])) {
        value = value * 10 + static_cast<std::uint64_t>(source[i] - '0');
        ++i;
      }
      Token token = makeToken(Token::Kind::IntLiteral, begin);
      token.text = source.substr(begin, i - begin);
      token.spelling = token.text;
      token.integer = static_cast<std::int64_t>(value);
      tokens.push_back(std::move(token));
      continue;
    }

    if (c == '"') {
      const std::size_t begin = i++;
      std::string text;
      for (;;) {
        if (i >= source.size()) throw Error("unterminated string literal", begin);

        const char ch = source[i++];
        if (ch == '"') break;

        if (ch != '\\') {
          text += ch;
          continue;
        }

        if (i >= source.size()) throw Error("unterminated string literal", begin);

        switch (source[i++]) {
          case 'n': text += '\n'; break;
          case 't': text += '\t'; break;
          case 'r': text += '\r'; break;
          case '\\': text += '\\'; break;
          case '"': text += '"'; break;
          case '0': text += '\0'; break;
          default: text += source[i - 1]; break;
        }
      }
      Token token = makeToken(Token::Kind::StringLiteral, begin);
      token.text = std::move(text);
      token.spelling = source.substr(begin, i - begin);
      tokens.push_back(std::move(token));
      continue;
    }

    // Two-character tokens.
    if (i + 1 < source.size()) {
      const std::string pair = source.substr(i, 2);
      if (pair == "->") {
        tokens.push_back(makeText(Token::Kind::Arrow, pair, i));
        i += 2;
        continue;
      }
      if (pair == "=>") {
        tokens.push_back(makeText(Token::Kind::FatArrow, pair, i));
        i += 2;
        continue;
      }
      if (pair == "==" || pair == "!=" || pair == ">=" || pair == "<=") {
        tokens.push_back(makeText(Token::Kind::Operator, pair, i));
        i += 2;
        continue;
      }
    }

    if (c == '=') {
      tokens.push_back(makeText(Token::Kind::Equals, "=", i++));
      continue;
    }

    if (isOperatorCharacter(c)) {
      tokens.push_back(makeText(Token::Kind::Operator, std::string(1, c), i++));
      continue;
    }

    throw Error(std::string("unexpected character '") + c + "'", i);
  }

  Token end = makeToken(Token::Kind::End, source.size());
  tokens.push_back(std::move(end));

  for (std::size_t j = 1; j < tokens.size(); ++j) {
    const Token& previous = tokens[j - 1];
    const std::size_t gapBegin = previous.position + previous.spelling.size();
    if (gapBegin < tokens[j].position) {
      const std::string gap = source.substr(gapBegin, tokens[j].position - gapBegin);
      tokens[j].lineBreakBefore = gap.find_first_of("\r\n") != std::string::npos;
    }
  }
  return tokens;
}

}  // namespace qwq
