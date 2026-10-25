#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace qwq {

/// Runs the interactive read-eval-print loop until end of input or `:quit`.
///
/// Each line is parsed, checked and, when its domain is `o`, evaluated.  A
/// line with unbalanced brackets is continued on the next line.
int runRepl(std::istream& input, std::ostream& output, std::ostream& errors,
            std::int64_t stepLimit, bool promptForInput = false,
            const std::vector<std::string>& searchPaths = {});

}  // namespace qwq
