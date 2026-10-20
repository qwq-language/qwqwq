#include "parser.hpp"

#include <algorithm>
#include <optional>

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

    while (startsTypeAlias() || startsDefinition()) {
      if (startsTypeAlias()) {
        program.typeAliases.push_back(parseTypeAlias());
      } else {
        program.definitions.push_back(parseDefinition());
      }
    }

    if (current().kind != Token::Kind::End) {
      program.main = parseExpr(0, true);
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

  /// A core is a literal or operator. A `[` starts a core only when it is the
  /// index operator `[]`; otherwise it starts an array type such as `[q]`.
  /// Array literals and constructors start with `{`.
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

  bool isOperator(const char* spelling) const {
    return current().kind == Token::Kind::Operator && current().text == spelling;
  }

  void expectOperator(const char* spelling, const char* description) {
    if (!isOperator(spelling)) {
      throw Error(std::string("expected ") + description, current().position);
    }
    ++index_;
  }

  bool startsOperatorFunctionName() const {
    const bool symbolic =
        peek().kind == Token::Kind::LeftParen &&
        peek(2).kind == Token::Kind::Operator &&
        peek(3).kind == Token::Kind::RightParen;
    const bool indexing =
        peek().kind == Token::Kind::LeftParen &&
        peek(2).kind == Token::Kind::LeftBracket &&
        peek(3).kind == Token::Kind::RightBracket &&
        peek(4).kind == Token::Kind::RightParen;
    return current().kind == Token::Kind::Identifier &&
           current().text == "operator" && (symbolic || indexing);
  }

  std::string parseFunctionName() {
    if (startsOperatorFunctionName()) {
      advance();
      expect(Token::Kind::LeftParen, "'(' after 'operator'");
      if (current().kind == Token::Kind::LeftBracket) {
        advance();
        expect(Token::Kind::RightBracket, "']' in the index operator name");
        expect(Token::Kind::RightParen, "')' after the index operator");
        return "operator([])";
      }
      const std::string symbol = advance().text;
      expect(Token::Kind::RightParen, "')' after the operator symbol");
      return "operator(" + symbol + ")";
    }
    if (current().kind != Token::Kind::Identifier) {
      throw Error("expected a function name", current().position);
    }
    return advance().text;
  }

  void parseTemplateParameters(std::vector<std::string>& parameters) {
    if (!isOperator("<")) return;
    advance();
    if (isOperator(">")) {
      throw Error("a function template must declare at least one parameter",
                  current().position);
    }
    for (;;) {
      if (current().kind != Token::Kind::Identifier) {
        throw Error("expected a template parameter name", current().position);
      }
      const std::string parameter = advance().text;
      if (std::find(parameters.begin(), parameters.end(), parameter) !=
          parameters.end()) {
        throw Error("duplicate template parameter '" + parameter + "'",
                    current().position);
      }
      parameters.push_back(parameter);
      if (current().kind != Token::Kind::Comma) break;
      advance();
      if (isOperator(">")) {
        throw Error("expected a template parameter after ','", current().position);
      }
    }
    expectOperator(">", "'>' after the function template parameters");
  }

  // ---- declarations -----------------------------------------------------

  bool startsTypeAlias() {
    if (current().kind != Token::Kind::Identifier) return false;
    if (peek().kind == Token::Kind::Equals) return true;
    if (peek().kind != Token::Kind::Operator || peek().text != "<") return false;

    const std::size_t save = index_;
    ++index_;  // name
    ++index_;  // '<'
    bool valid = current().kind == Token::Kind::Identifier;
    while (valid) {
      ++index_;  // parameter name
      if (current().kind == Token::Kind::Comma) {
        ++index_;
        valid = current().kind == Token::Kind::Identifier;
        continue;
      }
      break;
    }
    valid = valid && isOperator(">");
    if (valid) {
      ++index_;
      valid = current().kind == Token::Kind::Equals;
    }
    index_ = save;
    return valid;
  }

  TypeAliasDefinition parseTypeAlias() {
    TypeAliasDefinition alias;
    alias.position = current().position;
    alias.name = advance().text;

    if (isOperator("<")) {
      advance();
      if (isOperator(">")) {
        throw Error("a type template must declare at least one parameter",
                    current().position);
      }
      for (;;) {
        if (current().kind != Token::Kind::Identifier) {
          throw Error("expected a type parameter name", current().position);
        }
        const std::string parameter = advance().text;
        if (std::find(alias.parameters.begin(), alias.parameters.end(), parameter) !=
            alias.parameters.end()) {
          throw Error("duplicate type parameter '" + parameter + "'",
                      current().position);
        }
        alias.parameters.push_back(parameter);
        if (current().kind != Token::Kind::Comma) break;
        advance();
        if (isOperator(">")) {
          throw Error("expected a type parameter after ','", current().position);
        }
      }
      expectOperator(">", "'>' after the type parameters");
    }

    expect(Token::Kind::Equals, "'=' before the representation type");
    alias.body = parsePort();
    const std::size_t last = index_ == 0 ? 0 : index_ - 1;
    alias.end = tokens_[last].position + tokens_[last].spelling.size();
    return alias;
  }

  // ---- function definitions --------------------------------------------

  /// A statement is a definition when it is `name =>` or has a signature
  /// ending in `->`. Operator names use the spelling `operator(<)`.
  bool startsDefinition() {
    if (current().kind != Token::Kind::Identifier) return false;
    if (!startsOperatorFunctionName() && peek().kind != Token::Kind::Coerce &&
        peek().kind != Token::Kind::FatArrow &&
        !(peek().kind == Token::Kind::Operator && peek().text == "<")) {
      return false;
    }

    const std::size_t save = index_;
    bool result = false;
    try {
      parseFunctionName();
      std::vector<std::string> parameters;
      parseTemplateParameters(parameters);
      if (current().kind == Token::Kind::FatArrow) {
        result = true;
      } else if (current().kind == Token::Kind::Coerce) {
        advance();
        parsePort();
        result = current().kind == Token::Kind::Arrow;
      }
    } catch (const Error&) {
      result = false;
    }
    index_ = save;
    return result;
  }

  Definition parseDefinition() {
    Definition definition;
    definition.position = current().position;
    definition.name = parseFunctionName();
    parseTemplateParameters(definition.templateParameters);

    if (current().kind == Token::Kind::Coerce) {
      advance();
      definition.domain = parsePort();
      expect(Token::Kind::Arrow, "'->' in the signature");
      definition.codomain = parsePort();
      definition.hasSignature = true;
    }

    if (!definition.templateParameters.empty() && !definition.hasSignature) {
      throw Error("a function template requires an explicit signature",
                  definition.position);
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
      case Token::Kind::Underscore:
        return 3;
      default:
        return -1;
    }
  }

  static bool isRightAssociative(const Token& token) {
    return token.kind == Token::Kind::Loop || token.kind == Token::Kind::Branch;
  }

  ExprPtr parseExpr(int minimumPrecedence, bool allowImplicitComposition = false) {
    ExprPtr left = parseAtom();

    for (;;) {
      if (allowImplicitComposition && startsImplicitStage() &&
          sameLineAsPrevious()) {
        if (current().kind == Token::Kind::Identifier &&
            (startsTypeAlias() || startsDefinition())) {
          break;
        }
        constexpr int kImplicitCompositionPrecedence = 1;
        if (kImplicitCompositionPrecedence < minimumPrecedence) break;
        Token connector;
        connector.kind = Token::Kind::Compose;
        connector.position = current().position;
        ExprPtr right =
            parseExpr(kImplicitCompositionPrecedence + 1, allowImplicitComposition);
        left = makeConnector(connector, std::move(left), std::move(right));
        left->implicitComposition = true;
        continue;
      }

      const int prec = precedence(current());
      if (prec < minimumPrecedence) break;

      const Token connector = advance();
      const int nextMinimum = isRightAssociative(connector) ? prec : prec + 1;
      ExprPtr right = parseExpr(nextMinimum, allowImplicitComposition);
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
      case Token::Kind::Underscore: expr->kind = Expr::Kind::Append; break;
      case Token::Kind::Loop: expr->kind = Expr::Kind::Loop; break;
      case Token::Kind::Branch: expr->kind = Expr::Kind::Branch; break;
      default: throw Error("internal error: not a connector", token.position);
    }
    return expr;
  }

  // ---- atoms and faces --------------------------------------------------

  bool startsAnnotationPort() const {
    return isPortStart(current()) || current().kind == Token::Kind::LeftBracket ||
           current().kind == Token::Kind::LeftParen;
  }

  bool sameLineAsPrevious() const { return !current().lineBreakBefore; }

  bool startsImplicitStage() const {
    if (current().kind == Token::Kind::Identifier ||
        current().kind == Token::Kind::LeftParen || isPortStart(current()) ||
        startsCore()) {
      return true;
    }
    return current().kind == Token::Kind::LeftBracket &&
           peek().kind != Token::Kind::RightBracket;
  }

  bool isStructuralPort(const Port& port) const {
    for (const PortPart& part : port.parts) {
      if (part.kind == PortPart::Kind::Expression) return false;
      if ((part.kind == PortPart::Kind::Array ||
           part.kind == PortPart::Kind::Group) &&
          !isStructuralPort(*part.inner)) {
        return false;
      }
      for (const std::unique_ptr<Port>& argument : part.arguments) {
        if (!isStructuralPort(*argument)) return false;
      }
    }
    return true;
  }

  bool isSyntacticallyEmptyPort(const Port& port) const {
    for (const PortPart& part : port.parts) {
      if (part.kind == PortPart::Kind::Primitive) {
        if (!part.type.empty()) return false;
      } else if (part.kind == PortPart::Kind::Group) {
        if (!isSyntacticallyEmptyPort(*part.inner)) return false;
      } else {
        return false;
      }
    }
    return true;
  }

  bool startsLiteralCore() const {
    return current().kind == Token::Kind::IntLiteral ||
           current().kind == Token::Kind::StringLiteral ||
           current().kind == Token::Kind::LeftBrace;
  }

  Port parseAnnotationPort() {
    Port port;
    port.parts.push_back(parsePortPart());
    return port;
  }

  Port expressionAsPort(ExprPtr expression, std::size_t position) {
    Port port;
    PortPart part;
    part.kind = PortPart::Kind::Expression;
    part.expression = std::move(expression);
    part.position = position;
    port.parts.push_back(std::move(part));
    return port;
  }

  ExprPtr parseCall(std::optional<Port> domain, std::size_t position) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Call;
    expr->text = advance().text;
    expr->position = position;
    if (domain) {
      expr->domain = std::move(*domain);
      expr->hasDomain = true;
    }
    if (startsAnnotationPort() && sameLineAsPrevious()) {
      const std::size_t save = index_;
      try {
        Port codomain = parseAnnotationPort();
        if (isStructuralPort(codomain)) {
          expr->codomain = std::move(codomain);
          expr->hasCodomain = true;
        } else {
          index_ = save;
        }
      } catch (const Error&) {
        index_ = save;
      }
    }
    return expr;
  }

  ExprPtr parseAtom() {
    const std::size_t position = current().position;

    if (current().kind == Token::Kind::Identifier) {
      const std::size_t save = index_;
      const bool mayBeLessCore = peek().kind == Token::Kind::Operator &&
                                 peek().text == "<" &&
                                 !peek().lineBreakBefore;
      Port typeCandidate;
      try {
        typeCandidate = parseAnnotationPort();
      } catch (const Error&) {
        if (!mayBeLessCore) throw;
        index_ = save;
        PortPart simpleName;
        simpleName.kind = PortPart::Kind::Named;
        simpleName.name = advance().text;
        simpleName.position = position;
        typeCandidate.parts.push_back(std::move(simpleName));
      }
      const bool isTemplateType =
          !typeCandidate.parts.empty() &&
          typeCandidate.parts.front().kind == PortPart::Kind::Named &&
          !typeCandidate.parts.front().arguments.empty();

      if (isTemplateType) {
        auto expr = std::make_unique<Expr>();
        expr->kind = Expr::Kind::Call;
        expr->text = typeCandidate.parts.front().name;
        expr->position = position;
        for (std::unique_ptr<Port>& argument :
             typeCandidate.parts.front().arguments) {
          expr->templateArguments.push_back(std::move(*argument));
        }
        return expr;
      }

      index_ = save;
      return parseCall(std::nullopt, position);
    }

    if (current().kind == Token::Kind::LeftParen) {
      advance();
      const std::size_t innerStart = index_;
      try {
        Port domain = parsePort();
        if (current().kind == Token::Kind::RightParen) {
          advance();
          const bool literalInputNeedsIdentity =
              startsLiteralCore() && !isSyntacticallyEmptyPort(domain);
          if (isStructuralPort(domain) && startsCore() &&
              sameLineAsPrevious() && !literalInputNeedsIdentity) {
            return parseFace(std::move(domain), position);
          }
        }
      } catch (const Error&) {
        // Fall back to an ordinary parenthesised expression below.
      }
      index_ = innerStart;
      ExprPtr inner = parseExpr(0, true);
      expect(Token::Kind::RightParen, "')'");
      inner->parenthesized = true;
      return inner;
    }

    if (isPortStart(current()) ||
        (current().kind == Token::Kind::LeftBracket &&
         peek().kind != Token::Kind::RightBracket)) {
      Port domain = parseAnnotationPort();
      const bool literalInputNeedsIdentity =
          startsLiteralCore() && !isSyntacticallyEmptyPort(domain);
      if (startsCore() && sameLineAsPrevious() && !literalInputNeedsIdentity) {
        return parseFace(std::move(domain), position);
      }

      auto expr = std::make_unique<Expr>();
      expr->kind = Expr::Kind::Wire;
      expr->domain = std::move(domain);
      expr->hasDomain = true;
      expr->position = position;
      return expr;
    }

    if (startsCore()) return parseFace(std::nullopt, position);

    if (current().kind == Token::Kind::End) {
      throw Error("expected an expression, found end of input", current().position);
    }
    throw Error("expected an expression", current().position);
  }

  ExprPtr parseFace(std::optional<Port> domain, std::size_t position) {
    Core core = Core::Add;
    const std::size_t corePosition = current().position;
    std::int64_t integer = 0;
    std::string text;
    std::vector<ExprPtr> elements;
    parseCore(core, integer, text, elements);

    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Face;
    if (domain) {
      expr->domain = std::move(*domain);
      expr->hasDomain = true;
    }
    if (startsAnnotationPort() && sameLineAsPrevious()) {
      expr->codomain = parseAnnotationPort();
      expr->hasCodomain = true;
    }
    expr->core = core;
    expr->corePosition = corePosition;
    expr->integer = integer;
    expr->elements = std::move(elements);
    expr->text = std::move(text);
    expr->position = position;
    return expr;
  }

  ExprPtr parseFace(ExprPtr left, std::size_t position) {
    return parseFace(expressionAsPort(std::move(left), position), position);
  }

  /// Parses a comma-separated fixed array literal. Each element is one atom;
  /// a Pack-valued element is parenthesized, e.g. `{(1 _ 2), (3 _ 4)}`.
  /// Empty braces are also used for runtime-sized construction, as resolved by
  /// the checker from the input state.
  std::vector<ExprPtr> parseArrayLiteralItems() {
    expect(Token::Kind::LeftBrace, "'{'");
    std::vector<ExprPtr> items;
    if (current().kind != Token::Kind::RightBrace) {
      for (;;) {
        items.push_back(parseAtom());
        if (current().kind != Token::Kind::Comma) break;
        advance();
        if (current().kind == Token::Kind::RightBrace) break;  // trailing comma
      }
    }
    expect(Token::Kind::RightBrace, "'}'");
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
      throw Error("expected a core, i.e. a literal or operator", current().position);
    }

    const std::string symbol = current().text;

    // `o-3q`: a negative integer literal.
    if (symbol == "-" && peek().kind == Token::Kind::IntLiteral &&
        !peek().lineBreakBefore) {
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
    else if (symbol == "#") core = Core::Hash;
    else throw Error("unknown operator '" + symbol + "'");
  }

  // ---- ports ------------------------------------------------------------

  Port parsePort() {
    Port port;
    port.parts.push_back(parsePortPart());
    while (current().kind == Token::Kind::Underscore) {
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
      const std::size_t innerStart = index_;
      try {
        Port inner = parsePort();
        if (current().kind == Token::Kind::RightParen) {
          advance();
          PortPart part;
          part.kind = PortPart::Kind::Group;
          part.inner = std::make_unique<Port>(std::move(inner));
          part.position = position;
          return part;
        }
      } catch (const Error&) {
        // A parenthesised morphism remains supported where an identity
        // expression is used as a type group.
      }
      index_ = innerStart;
      ExprPtr inner = parseExpr(0, true);
      expect(Token::Kind::RightParen, "')'");

      PortPart part;
      part.kind = PortPart::Kind::Expression;
      part.expression = std::move(inner);
      part.position = position;
      return part;
    }

    if (current().kind == Token::Kind::Identifier) {
      PortPart part;
      part.kind = PortPart::Kind::Named;
      part.name = advance().text;
      part.position = position;

      if (isOperator("<")) {
        advance();
        if (isOperator(">")) {
          throw Error("a type application must include at least one argument",
                      current().position);
        }
        for (;;) {
          Port argument = parsePort();
          part.arguments.push_back(
              std::make_unique<Port>(std::move(argument)));
          if (current().kind != Token::Kind::Comma) break;
          advance();
          if (isOperator(">")) {
            throw Error("expected a type argument after ','", current().position);
          }
        }
        expectOperator(">", "'>' after the type arguments");
      }
      return part;
    }

    if (isPortStart(current())) {
      PortPart part;
      part.kind = PortPart::Kind::Primitive;
      part.position = position;

      switch (current().kind) {
        case Token::Kind::VoidPort: break;  // the empty stream type
        case Token::Kind::IntPort: part.type = StreamType(intType()); break;
        case Token::Kind::StrPort: part.type = StreamType(strType()); break;
        case Token::Kind::BoolPort: part.type = StreamType(boolType()); break;
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
