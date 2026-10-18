#include "diagnostic.hpp"

#include <ostream>

namespace qwq {

void reportError(std::ostream& errors, const std::string& source, const Error& error) {
  errors << "error: " << error.what() << "\n";

  const std::size_t position = error.position();
  if (position == Error::npos || position > source.size()) return;

  std::size_t lineBegin = position;
  while (lineBegin > 0 && source[lineBegin - 1] != '\n') --lineBegin;

  std::size_t lineEnd = source.find('\n', position);
  if (lineEnd == std::string::npos) lineEnd = source.size();

  std::size_t lineNumber = 1;
  for (std::size_t i = 0; i < lineBegin; ++i) {
    if (source[i] == '\n') ++lineNumber;
  }

  const std::string number = std::to_string(lineNumber);

  errors << " " << number << " | " << source.substr(lineBegin, lineEnd - lineBegin)
         << "\n";
  errors << " " << std::string(number.size(), ' ') << " | "
         << std::string(position - lineBegin, ' ') << "^\n";
}

}  // namespace qwq
