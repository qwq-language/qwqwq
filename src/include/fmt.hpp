#pragma once

#include <string>

namespace qwq {

/// Rewrites `source` into canonical form:
///
///   * one space between items, no padding inside brackets, list commas formatted
///     as `, `, and underscores glued to both adjacent items in Pack and append
///     contexts;
///   * `#` is spaced like an operator for Pack reshaping, and a `=> header.awa`
///     line is reproduced as written, one per line;
///   * template types use `Pair<X, Y>`; array literals/constructors stay glued;
///   * a chain of stages (`~ @ ?`) whose two operands hold no chain of their
///     own is a leaf and stays on one line; a longer chain, or one holding
///     another chain, is written as a block: the brackets get a line of their
///     own and one operand goes on each line, indented one tab per enclosing
///     block.  Brackets that merely wrap a block add neither a line nor a tab;
///   * nothing is broken for width alone -- the shape of the program, not its
///     length, decides where a line ends;
///   * a definition whose body needs one line keeps the header with it,
///     otherwise the body follows `=>` on a line of its own, indented one tab;
///   * one blank line between definitions, none at the start or the end;
///   * comments keep their place: their own line, or trailing after two spaces.
///
/// Throws `Error` if the source does not lex or parse. `allowDeclarations` is
/// set when the source is a `.awa` header, whose definitions may stop at the
/// signature.
std::string format(const std::string& source, bool allowDeclarations = false);

}  // namespace qwq
