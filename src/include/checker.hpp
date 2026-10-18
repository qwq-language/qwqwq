#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ast.hpp"

namespace qwq {

/// Signature of a named morphism: its declared domain and codomain words.
using Signature = std::pair<Word, Word>;

/// Every function may have several overloads.  Two overloads of the same name
/// must differ in their domain, so a call is resolved by the type of the
/// stream it receives — the same rule that lets `-` mean both negation and
/// subtraction.
using Overloads = std::vector<Signature>;

/// The set of names in scope.
using Environment = std::map<std::string, Overloads>;

/// Type checks a program in place: every definition gets its resolved
/// signature, every node its `in` and `out` words, overloaded calls are
/// resolved, and ambiguous cores are normalised.
///
/// Definitions are visible to the definitions that follow them and to their
/// own body, which is what makes recursion possible.  Throws Error with the
/// position of the offending node.
void check(Program& program);

/// Type checks a standalone expression with `environment` in scope, without
/// checking the definitions themselves.  Used by the REPL, where definitions
/// have already been validated line by line.
void check(Expr& expression, const Environment& environment = {});

}  // namespace qwq
