#pragma once

#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

namespace qwq {

/// Parses a token stream produced by `tokenize` into definitions plus an
/// optional main expression.
///
///   Program     ::= Definition* Expr?
///   Definition  ::= "def" Identifier ":" Port "->" Port ":=" Expr
///
/// Precedence, from loosest to tightest binding:
///
///   1  ~  ;  :        left associative
///   2  @  ?           right associative
///   3  _              left associative  (append)
///   4  faces and cores
///
/// A core binds tighter than every connector: inside a face, `_` still binds
/// tighter than the core, so `q $ q_q` has the two-slot word `q_q` as its
/// codomain.  A bare identifier is a call to a previously defined morphism.
Program parse(const std::vector<Token>& tokens);

}  // namespace qwq
