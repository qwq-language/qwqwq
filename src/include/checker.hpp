#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ast.hpp"

namespace qwq {

/// Signature of a named morphism: one input stream type and one output stream type.
using Signature = std::pair<StreamType, StreamType>;

/// A function overload may be concrete or parameterized by type variables.
struct FunctionOverload {
  Signature signature;
  std::vector<std::string> templateParameters;
  const Definition* definition = nullptr;
};

using Overloads = std::vector<FunctionOverload>;
using TypeBindings = std::map<std::string, StreamType>;

/// Names in scope. Class types are kept separate from functions so a
/// structural type name is interpreted only in a port/type position.
struct Environment {
  std::map<std::string, Overloads> functions;
  std::map<std::string, const TypeAliasDefinition*> typeAliases;
  TypeBindings typeBindings;
  std::vector<Definition>* specializations = nullptr;
  std::vector<std::string> activeSpecializations;
};

/// Type checks a program in place: every definition gets its resolved
/// signature, every node its input and output stream types, overloaded calls are
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
