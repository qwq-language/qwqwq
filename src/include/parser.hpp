#pragma once

#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

namespace qwq {

/// Parses a token stream produced by `tokenize` into nominal class types,
/// function definitions, and an optional main expression.
///
///   Program     ::= (ClassType | Definition)* Expr?
///   ClassType   ::= Identifier ["<" Identifier ("," Identifier)* ">"] "=" Port
///   Definition  ::= FunctionName ["<" Identifier ("," Identifier)* ">"]
///                   ["(" Port "->" Port ")"] "=>" Expr
///   FunctionName ::= Identifier | "operator(" Operator ")" | "operator([])"
///
/// Precedence, from loosest to tightest binding:
///
///   1  ~  ;  :        left associative
///   2  @  ?           right associative
///   3  _              left associative  (sequential stream append)
///   4  faces and cores
///
/// Same-line adjacent stages compose implicitly in a main expression and
/// inside a parenthesized group. At the top level of a function body, use `~`
/// or parentheses to join adjacent stages, so the body does not absorb the
/// following program expression. Adjacent bare wires and literal-stream
/// groups still require an explicit `~`; for example, `q q` is rejected while
/// `q ~ q` is preserved. A core binds tighter than every connector.
/// `_` is left-associative in both contexts: it forms a binary Pack in a port
/// and a sequential append in an expression; whitespace does not distinguish
/// the two. Commas remain separators in template parameters, type arguments,
/// and array lists. `#` is selected by type as either input Read or Pack
/// reshaping. In expression position, a bare
/// identifier names a morphism, or the identity on a non-template class type
/// when no morphism of that name exists.
Program parse(const std::vector<Token>& tokens);

}  // namespace qwq
