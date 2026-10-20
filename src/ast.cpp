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

std::string renderPortPart(const PortPart& part) {
  switch (part.kind) {
    case PortPart::Kind::Primitive:
      return toString(part.type);
    case PortPart::Kind::Array:
      return "[" + portSource(*part.inner) + "]";
    case PortPart::Kind::Group:
      return "(" + portSource(*part.inner) + ")";
    case PortPart::Kind::Named: {
      std::string result = part.name;
      if (!part.arguments.empty()) {
        result += "<";
        for (std::size_t i = 0; i < part.arguments.size(); ++i) {
          if (i != 0) result += ", ";
          result += portSource(*part.arguments[i]);
        }
        result += ">";
      }
      return result;
    }
    case PortPart::Kind::Expression:
      return "(<identity expression>)";
  }
  return "<?>";
}

std::string aliasSource(const TypeAliasDefinition& alias) {
  std::string result = alias.name;
  if (!alias.parameters.empty()) {
    result += "<";
    for (std::size_t i = 0; i < alias.parameters.size(); ++i) {
      if (i != 0) result += ", ";
      result += alias.parameters[i];
    }
    result += ">";
  }
  return result + " = " + portSource(alias.body);
}

std::string functionNameSource(const Definition& definition) {
  std::string result = definition.name;
  if (!definition.templateParameters.empty()) {
    result += "<";
    for (std::size_t i = 0; i < definition.templateParameters.size(); ++i) {
      if (i != 0) result += ", ";
      result += definition.templateParameters[i];
    }
    result += ">";
  }
  return result;
}

}  // namespace

std::string portSource(const Port& port) {
  std::string result;
  for (const PortPart& part : port.parts) {
    if (!result.empty()) result += "_";
    result += renderPortPart(part);
  }
  return result.empty() ? "o" : result;
}

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
    case Core::Hash: return "#";
    case Core::Reshape: return "#";
    case Core::Index: return "index";
    case Core::ArrayWrite: return "write";
    case Core::ArrayLength: return "length";
    case Core::ArrayLiteral: return "array-literal";
    case Core::ArrayFill: return "array-fill";
    case Core::ArrayDefault: return "array-default";
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
      if (!expr.templateArguments.empty()) {
        out << "<";
        for (std::size_t i = 0; i < expr.templateArguments.size(); ++i) {
          if (i != 0) out << ", ";
          out << portSource(expr.templateArguments[i]);
        }
        out << ">";
      }
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

  out << "   [" << toString(expr.in) << " -> "
      << toString(expr.out) << "]\n";

  if (expr.left) dump(*expr.left, out, indent + 1);
  if (expr.right) dump(*expr.right, out, indent + 1);
  for (const ExprPtr& item : expr.elements) dump(*item, out, indent + 1);
}

void dump(const Program& program, std::ostream& out) {
  std::size_t aliasIndex = 0;
  std::size_t definitionIndex = 0;
  while (aliasIndex < program.typeAliases.size() ||
         definitionIndex < program.definitions.size()) {
    const bool printAlias =
        definitionIndex == program.definitions.size() ||
        (aliasIndex < program.typeAliases.size() &&
         program.typeAliases[aliasIndex].position <
             program.definitions[definitionIndex].position);
    if (printAlias) {
      out << aliasSource(program.typeAliases[aliasIndex++]) << "\n";
      continue;
    }

    const Definition& definition = program.definitions[definitionIndex++];
    out << functionNameSource(definition) << " : " << toString(definition.in) << " -> "
        << toString(definition.out) << "\n";
    if (definition.body) {
      if (definition.templateParameters.empty()) {
        dump(*definition.body, out, 1);
      } else {
        out << "  template body (type-checked when instantiated)\n";
      }
    }
  }
  if (program.main) dump(*program.main, out);
}

}  // namespace qwq
