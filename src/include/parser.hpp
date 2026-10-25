#pragma once

#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

namespace qwq {

/// Parses a token stream produced by `tokenize` into nominal class types,
/// function definitions, and an optional main expression.
///
///   Program     ::= (Include | ClassType | Definition)* Expr?
///   Include     ::= "=>" Path                     (one per line, `.awa`)
///   ClassType   ::= Identifier ["<" Identifier ("," Identifier)* ">"] "=" Port
///   Definition  ::= FunctionName ["<" Identifier ("," Identifier)* ">"]
///                   ["(" Port "->" Port ")"] "=>" Expr
///   Declaration ::= FunctionName ["<" Identifier ("," Identifier)* ">"]
///                   "(" Port "->" Port ")"        (headers only: no body)
///   FunctionName ::= Identifier | "operator(" Operator ")" | "operator([])"
///
/// Precedence, from loosest to tightest binding:
///
///   1  ~  :           left associative
///   2  @  ?           right associative
///   3  _              left associative  (sequential stream append)
///   4  faces and cores
///
/// Same-line adjacent stages compose implicitly in a main expression and
/// inside a parenthesized group. At the top level of a function body, use `~`
/// or parentheses to join adjacent stages, so the body does not absorb the
/// following program expression. A core binds tighter than every connector.
/// `_` is left-associative in both contexts: it forms a binary Pack in a port
/// and a sequential append in an expression; whitespace does not distinguish
/// the two. Commas remain separators in template parameters, type arguments,
/// and array lists. `#` regroups Pack structure. In expression position, a bare
/// identifier names a morphism, or the identity on a non-template class type
/// when no morphism of that name exists.
/// `allowDeclarations` is set for `.awa` headers, where a definition may stop
/// after its signature: the body is then provided by the runtime.
Program parse(const std::vector<Token>& tokens, bool allowDeclarations = false);

}  // namespace qwq
