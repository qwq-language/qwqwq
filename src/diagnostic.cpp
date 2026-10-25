#include "diagnostic.hpp"

#include "color.hpp"

#include <ostream>

namespace qwq {

void reportError(std::ostream& errors, const std::string& source, const Error& error) {
  errors << color::bold(true) << color::red(true) << "error:" << color::reset(true) << ' '
         << error.what() << "\n";

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

  errors << color::dim(true) << ' ' << number << " | " << color::reset(true)
         << source.substr(lineBegin, lineEnd - lineBegin) << "\n";
  errors << color::dim(true) << ' ' << std::string(number.size(), ' ') << " | " << color::reset(true)
         << std::string(position - lineBegin, ' ') << color::bold(true) << color::red(true) << '^'
         << color::reset(true) << "\n";
}

}  // namespace qwq
