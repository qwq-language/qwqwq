#include "parser.hpp"

#include <algorithm>

#include "error.hpp"

namespace qwq {
namespace {

/// Two's-complement negation, so that -INT64_MIN wraps instead of overflowing.
std::int64_t negate(std::int64_t value) {
  return static_cast<std::int64_t>(0u - static_cast<std::uint64_t>(value));
}

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  Program parseProgram() {
    Program program;

    while (startsDefinition()) {
      program.definitions.push_back(parseDefinition());
    }

    if (current().kind != Token::Kind::End) {
      program.main = parseExpr(0);
      if (current().kind != Token::Kind::End) {
        throw Error("unexpected trailing input", current().position);
      }
    }

    return program;
  }

 private:
  std::vector<Token> tokens_;
  std::size_t index_ = 0;

  const Token& current() const { return tokens_[index_]; }

  const Token& peek(std::size_t offset = 1) const {
    return tokens_[std::min(index_ + offset, tokens_.size() - 1)];
  }

  const Token& advance() { return tokens_[index_++]; }

  /// A core is a literal or an operator.  A `[` starts a core only when it is
  /// the index operator `[]`; otherwise it starts an array type such as
  /// `[q]`.  An array literal starts with `{` and is a core start already.
  bool startsCore() const {
    if (isCoreStart(current())) return true;
    if (current().kind != Token::Kind::LeftBracket) return false;
    return peek().kind == Token::Kind::RightBracket;
  }

  void expect(Token::Kind kind, const char* description) {
    if (current().kind != kind) {
      throw Error(std::string("expected ") + description, current().position);
    }
    ++index_;
  }

  // ---- definitions ------------------------------------------------------

  /// A statement is a definition when it is `name =>`, or `name : A -> B =>`.
  /// `->` is only legal inside a signature, so the port after a `:` decides
  /// between a definition and a program that coerces a call (`f : o`).
  bool startsDefinition() {
    if (current().kind != Token::Kind::Identifier) return false;
    if (peek().kind == Token::Kind::FatArrow) return true;
    if (peek().kind != Token::Kind::Coerce) return false;

    const std::size_t save = index_;
    index_ += 2;  // name and ':'
    bool signature = false;
    try {
      parsePort();
      signature = current().kind == Token::Kind::Arrow;
    } catch (const Error&) {
      signature = false;
    }
    index_ = save;
    return signature;
  }

  Definition parseDefinition() {
    Definition definition;
    definition.position = current().position;

    if (current().kind != Token::Kind::Identifier) {
      throw Error("expected a name at the start of a definition",
                  current().position);
    }
    definition.name = advance().text;

    if (current().kind == Token::Kind::Coerce) {
      advance();
      definition.domain = parsePort();
      expect(Token::Kind::Arrow, "'->' in the signature");
      definition.codomain = parsePort();
      definition.hasSignature = true;
    }

    expect(Token::Kind::FatArrow, "'=>' before the body");

    definition.body = parseExpr(0);
    const std::size_t last = index_ == 0 ? 0 : index_ - 1;
    definition.end = tokens_[last].position + tokens_[last].spelling.size();
    return definition;
  }

  // ---- precedence -------------------------------------------------------

  static int precedence(const Token& token) {
    switch (token.kind) {
      case Token::Kind::Compose:
      case Token::Kind::Tee:
      case Token::Kind::Coerce:
        return 1;
      case Token::Kind::Loop:
      case Token::Kind::Branch:
        return 2;
      case Token::Kind::Append:
        return 3;
      default:
        return -1;
    }
  }

  static bool isRightAssociative(const Token& token) {
    return token.kind == Token::Kind::Loop || token.kind == Token::Kind::Branch;
  }

  ExprPtr parseExpr(int minimumPrecedence) {
    ExprPtr left = parseAtom();

    for (;;) {
      const int prec = precedence(current());
      if (prec < minimumPrecedence) break;

      const Token connector = advance();
      const int nextMinimum = isRightAssociative(connector) ? prec : prec + 1;
      ExprPtr right = parseExpr(nextMinimum);
      left = makeConnector(connector, std::move(left), std::move(right));
    }
    return left;
  }

  static ExprPtr makeConnector(const Token& token, ExprPtr left, ExprPtr right) {
    auto expr = std::make_unique<Expr>();
    expr->position = token.position;
    expr->left = std::move(left);
    expr->right = std::move(right);

    switch (token.kind) {
      case Token::Kind::Compose: expr->kind = Expr::Kind::Compose; break;
      case Token::Kind::Tee: expr->kind = Expr::Kind::Tee; break;
      case Token::Kind::Coerce: expr->kind = Expr::Kind::Coerce; break;
      case Token::Kind::Append: expr->kind = Expr::Kind::Append; break;
      case Token::Kind::Loop: expr->kind = Expr::Kind::Loop; break;
      case Token::Kind::Branch: expr->kind = Expr::Kind::Branch; break;
      default: throw Error("internal error: not a connector", token.position);
    }
    return expr;
  }

  // ---- atoms and faces --------------------------------------------------

  ExprPtr parseAtom() {
    const std::size_t position = current().position;

    if (current().kind == Token::Kind::Identifier) {
      auto expr = std::make_unique<Expr>();
      expr->kind = Expr::Kind::Call;
      expr->text = current().text;
      expr->position = position;
      advance();
      return expr;
    }

    if (current().kind == Token::Kind::LeftParen) {
      advance();
      ExprPtr inner = parseExpr(0);
      expect(Token::Kind::RightParen, "')'");

      // `(q_q)` followed by a core is the domain of a face, not a group.
      if (startsCore()) return parseFace(std::move(inner), position);

      // `(q_[q])_q`: the group is the first item of a port.
      if (current().kind == Token::Kind::Join) {
        Port port;
        PortPart part;
        part.kind = PortPart::Kind::Expression;
        part.expression = std::move(inner);
        part.position = position;
        port.parts.push_back(std::move(part));

        while (current().kind == Token::Kind::Join) {
          advance();
          port.parts.push_back(parsePortPart());
        }

        if (startsCore()) return parseFace(std::move(port), position);

        auto expr = std::make_unique<Expr>();
        expr->kind = Expr::Kind::Wire;
        expr->domain = std::move(port);
        expr->position = position;
        return expr;
      }

      return inner;
    }

    if (isPortStart(current()) || current().kind == Token::Kind::LeftBracket) {
      Port domain = parsePort();
      if (startsCore()) return parseFace(std::move(domain), position);

      auto expr = std::make_unique<Expr>();
      expr->kind = Expr::Kind::Wire;
      expr->domain = std::move(domain);
      expr->position = position;
      return expr;
    }

    if (current().kind == Token::Kind::End) {
      throw Error("expected an expression, found end of input", current().position);
    }
    throw Error("expected an expression", current().position);
  }

  ExprPtr parseFace(Port domain, std::size_t position) {
    Core core = Core::Add;
    std::int64_t integer = 0;
    std::string text;
    std::vector<ExprPtr> elements;
    parseCore(core, integer, text, elements);

    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Face;
    expr->domain = std::move(domain);
    expr->codomain = parsePort();
    expr->core = core;
    expr->integer = integer;
    expr->elements = std::move(elements);
    expr->text = std::move(text);
    expr->position = position;
    return expr;
  }

  ExprPtr parseFace(ExprPtr left, std::size_t position) {
    Port domain;
    PortPart part;
    part.kind = PortPart::Kind::Expression;
    part.expression = std::move(left);
    part.position = position;
    domain.parts.push_back(std::move(part));
    return parseFace(std::move(domain), position);
  }

  /// Parses `{ ... }` after the opening brace has been seen: one atom per
  /// element, possibly none.  The elements are written as the literal faces
  /// they are (`o1q`, `o"text"s`, a nested `o{...}[q]`), never as bare cores;
  /// the checker rejects anything that is not a closed literal face.
  std::vector<ExprPtr> parseArrayLiteralItems() {
    expect(Token::Kind::LeftBrace, "'{'");
    std::vector<ExprPtr> items;
    while (current().kind != Token::Kind::RightBrace) {
      items.push_back(parseAtom());
    }
    advance();  // '}'
    return items;
  }

  void parseCore(Core& core, std::int64_t& integer, std::string& text,
                 std::vector<ExprPtr>& elements) {
    if (current().kind == Token::Kind::LeftBracket) {
      advance();
      expect(Token::Kind::RightBracket, "']'");
      core = Core::Index;
      return;
    }

    if (current().kind == Token::Kind::LeftBrace) {
      elements = parseArrayLiteralItems();
      core = Core::ArrayLiteral;
      return;
    }

    if (current().kind == Token::Kind::IntLiteral) {
      integer = current().integer;
      advance();
      core = Core::IntLiteral;
      return;
    }

    if (current().kind == Token::Kind::StringLiteral) {
      text = current().text;
      advance();
      core = Core::StringLiteral;
      return;
    }

    if (current().kind != Token::Kind::Operator) {
      throw Error("expected a core, i.e. a literal or an operator", current().position);
    }

    const std::string symbol = current().text;

    // `o-3q`: a negative integer literal.
    if (symbol == "-" && peek().kind == Token::Kind::IntLiteral) {
      advance();
      integer = negate(current().integer);
      advance();
      core = Core::IntLiteral;
      return;
    }

    advance();

    if (symbol == "+") core = Core::Add;
    else if (symbol == "-") core = Core::Negate;  // narrowed to Sub by the checker
    else if (symbol == "*") core = Core::Mul;
    else if (symbol == "/") core = Core::Div;
    else if (symbol == "%") core = Core::Mod;
    else if (symbol == "|") core = Core::BitOr;
    else if (symbol == "&") core = Core::BitAnd;
    else if (symbol == "^") core = Core::BitXor;
    else if (symbol == "!") core = Core::Not;
    else if (symbol == "==") core = Core::Equal;
    else if (symbol == "!=") core = Core::NotEqual;
    else if (symbol == ">") core = Core::Greater;
    else if (symbol == "<") core = Core::Less;
    else if (symbol == ">=") core = Core::GreaterEqual;
    else if (symbol == "<=") core = Core::LessEqual;
    else if (symbol == "$") core = Core::Duplicate;
    else if (symbol == ".") core = Core::Swap;
    else if (symbol == "#") core = Core::Read;
    else throw Error("unknown operator '" + symbol + "'");
  }

  // ---- ports ------------------------------------------------------------

  Port parsePort() {
    Port port;
    port.parts.push_back(parsePortPart());
    while (current().kind == Token::Kind::Join) {
      advance();
      port.parts.push_back(parsePortPart());
    }
    return port;
  }

  PortPart parsePortPart() {
    const std::size_t position = current().position;

    if (current().kind == Token::Kind::LeftBracket) {
      advance();
      Port inner = parsePort();
      expect(Token::Kind::RightBracket, "']'");

      PortPart part;
      part.kind = PortPart::Kind::Array;
      part.inner = std::make_unique<Port>(std::move(inner));
      part.position = position;
      return part;
    }

    if (current().kind == Token::Kind::LeftParen) {
      advance();
      ExprPtr inner = parseExpr(0);
      expect(Token::Kind::RightParen, "')'");

      PortPart part;
      part.kind = PortPart::Kind::Expression;
      part.expression = std::move(inner);
      part.position = position;
      return part;
    }

    if (isPortStart(current())) {
      PortPart part;
      part.kind = PortPart::Kind::Word;
      part.position = position;

      switch (current().kind) {
        case Token::Kind::VoidPort: break;  // the empty word
        case Token::Kind::IntPort: part.word.push_back(intType()); break;
        case Token::Kind::StrPort: part.word.push_back(strType()); break;
        case Token::Kind::BoolPort: part.word.push_back(boolType()); break;
        default: break;
      }
      advance();
      return part;
    }

    throw Error("expected a port, i.e. a type", current().position);
  }
};

}  // namespace

Program parse(const std::vector<Token>& tokens) {
  // Comments carry no syntax; the parser only sees code.
  std::vector<Token> code;
  code.reserve(tokens.size());
  for (const Token& token : tokens) {
    if (token.kind != Token::Kind::Comment) code.push_back(token);
  }
  return Parser(code).parseProgram();
}

}  // namespace qwq
