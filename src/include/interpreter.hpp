#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "ast.hpp"
#include "value.hpp"

namespace qwq {

/// Evaluates a checked program as a queue machine.
///
/// Every node consumes exactly `|expr.in|` values and produces exactly
/// `|expr.out|` values, so the whole program is a left-to-right rewrite of
/// one ordered stream.  There is no environment and no mutable state; named
/// morphisms are expanded by substitution, guarded by a depth limit.
class Interpreter {
 public:
  Interpreter(std::ostream& output, std::istream& input,
              std::int64_t stepLimit,
              const std::vector<Definition>& definitions);

  /// Runs `expr` on `input`.  Throws Error on runtime faults (division by
  /// zero, step limit exceeded, runaway recursion).
  Stream run(const Expr& expr, Stream input);

 private:
  /// One overload: the checked signature plus the body to expand.
  struct Function {
    Word in;
    Word out;
    const Expr* body;
  };

  std::ostream& output_;
  std::istream& input_;
  std::int64_t stepLimit_;
  std::int64_t steps_ = 0;
  std::map<std::string, std::vector<Function>> functions_;

  Stream evaluate(const Expr& expr, Stream input);
  Stream evaluateFace(const Expr& expr, const Stream& input);
  Stream evaluateCall(const Expr& expr, Stream input);
  /// Reads one whitespace-delimited token from the input source and turns it
  /// into the value `#` promised.
  Value readInput(const Expr& expr);

  void tick(const Expr& expr);
  static Value applyBinary(Core core, const Value& left, const Value& right,
                           std::size_t position);
};

}  // namespace qwq
