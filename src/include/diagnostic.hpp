#pragma once

#include <iosfwd>
#include <string>

#include "error.hpp"

namespace qwq {

/// Prints `error: <message>` followed by the offending source line and a
/// caret pointing at the reported position.
void reportError(std::ostream& errors, const std::string& source, const Error& error);

}  // namespace qwq
