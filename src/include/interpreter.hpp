#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "ast.hpp"
#include "value.hpp"

namespace qwq {

/// Evaluates a checked program over streams.
///
/// Every node consumes and produces one stream. A stream is either empty or
/// contains one value; a pack is itself one value and holds exactly two
/// components. Named morphisms are expanded by substitution.
class Interpreter {
 public:
  Interpreter(std::ostream& output, std::istream& input,
              std::int64_t stepLimit,
              const std::vector<Definition>& definitions,
              std::ostream* inputPrompt = nullptr,
              const std::vector<Definition>* specializations = nullptr);

  /// Number of external input reads performed during the last run.
  std::size_t inputReadCount() const noexcept { return inputReadCount_; }

  /// Runs `expr` on `input`.  Throws Error on runtime faults (division by
  /// zero, step limit exceeded, runaway recursion).
  Stream run(const Expr& expr, Stream input);

 private:
  /// One overload: the checked signature plus the body to expand.
  struct Function {
    StreamType in;
    StreamType out;
    const Expr* body;
    std::string templateInstanceKey;
  };

  std::ostream& output_;
  std::istream& input_;
  std::ostream* inputPrompt_;
  std::int64_t stepLimit_;
  std::int64_t steps_ = 0;
  std::size_t inputReadCount_ = 0;
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
