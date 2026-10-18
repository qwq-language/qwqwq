#include "ast.hpp"

#include <ostream>

namespace qwq {
namespace {

void quote(std::ostream& out, const std::string& text) {
  out << '"';
  for (const char c : text) {
    switch (c) {
      case '\n': out << "\\n"; break;
      case '\t': out << "\\t"; break;
      case '\r': out << "\\r"; break;
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      default: out << c; break;
    }
  }
  out << '"';
}

}  // namespace

const char* name(Core core) {
  switch (core) {
    case Core::IntLiteral: return "int-literal";
    case Core::StringLiteral: return "str-literal";
    case Core::BoolLiteral: return "bool-literal";
    case Core::Negate: return "negate";
    case Core::Not: return "not";
    case Core::Add: return "add";
    case Core::Sub: return "subtract";
    case Core::Mul: return "multiply";
    case Core::Div: return "divide";
    case Core::Mod: return "modulo";
    case Core::BitOr: return "or";
    case Core::BitAnd: return "and";
    case Core::BitXor: return "xor";
    case Core::Equal: return "equal";
    case Core::NotEqual: return "not-equal";
    case Core::Greater: return "greater";
    case Core::Less: return "less";
    case Core::GreaterEqual: return "greater-equal";
    case Core::LessEqual: return "less-equal";
    case Core::Duplicate: return "duplicate";
    case Core::Swap: return "swap";
    case Core::Index: return "index";
    case Core::ArrayWrite: return "write";
    case Core::ArrayLength: return "length";
    case Core::ArrayLiteral: return "array-literal";
    case Core::Read: return "read";
  }
  return "?";
}

void dump(const Expr& expr, std::ostream& out, int indent) {
  out << std::string(static_cast<std::size_t>(indent) * 2, ' ');

  switch (expr.kind) {
    case Expr::Kind::Wire:
      out << "wire";
      break;
    case Expr::Kind::Call:
      out << "call " << expr.text;
      break;
    case Expr::Kind::Face:
      out << name(expr.core);
      if (expr.core == Core::IntLiteral) out << ' ' << expr.integer;
      if (expr.core == Core::BoolLiteral) out << ' ' << (expr.integer != 0 ? 1 : 0);
      if (expr.core == Core::StringLiteral) {
        out << ' ';
        quote(out, expr.text);
      }
      break;
    case Expr::Kind::Compose: out << "~"; break;
    case Expr::Kind::Tee: out << ";"; break;
    case Expr::Kind::Coerce: out << ":"; break;
    case Expr::Kind::Append: out << "_"; break;
    case Expr::Kind::Loop: out << "@"; break;
    case Expr::Kind::Branch: out << "?"; break;
  }

  out << "   [" << toString(expr.in, expr.domainItems) << " -> "
      << toString(expr.out, expr.codomainItems) << "]\n";

  if (expr.left) dump(*expr.left, out, indent + 1);
  if (expr.right) dump(*expr.right, out, indent + 1);
  for (const ExprPtr& item : expr.elements) dump(*item, out, indent + 1);
}

void dump(const Program& program, std::ostream& out) {
  for (const Definition& definition : program.definitions) {
    out << definition.name << " : "
        << toString(definition.in, definition.domainItems) << " -> "
        << toString(definition.out, definition.codomainItems) << "\n";
    if (definition.body) dump(*definition.body, out, 1);
  }
  if (program.main) dump(*program.main, out);
}

}  // namespace qwq
