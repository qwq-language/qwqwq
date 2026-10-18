#include "fmt.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ast.hpp"
#include "error.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace qwq {
namespace {

bool isOpening(const Token& token) {
  return token.kind == Token::Kind::LeftParen || token.kind == Token::Kind::LeftBracket ||
         token.kind == Token::Kind::LeftBrace;
}

bool isClosing(const Token& token) {
  return token.kind == Token::Kind::RightParen || token.kind == Token::Kind::RightBracket ||
         token.kind == Token::Kind::RightBrace;
}

bool isPort(const Token& token) {
  switch (token.kind) {
    case Token::Kind::VoidPort:
    case Token::Kind::IntPort:
    case Token::Kind::StrPort:
    case Token::Kind::BoolPort: return true;
    default: return false;
  }
}

/// `~ ; @ ?` -- the connectors that hold a chain of stages together.
/// `:` and `_` are left out on purpose: `(q : o)` and `[q]_q_q` are ordinary
/// faces and port words, and letting them open a block would bury the shape
/// of the program under noise.
bool isLayoutTrigger(const Token& token) {
  switch (token.kind) {
    case Token::Kind::Compose:
    case Token::Kind::Tee:
    case Token::Kind::Loop:
    case Token::Kind::Branch: return true;
    default: return false;
  }
}

bool touching(const Token& left, const Token& right) {
  return left.position + left.spelling.size() == right.position;
}

/// Which pairs of tokens are written without a space between them.
///
/// Whitespace is insignificant everywhere except around `_`, so these pairs are
/// a matter of taste, not of meaning: they keep the idioms of the language
/// (`o5q`, `o"text"s`, `o-3q`, `o{o1q o2q o3q}[q]`, `q_q`) in one piece.
std::vector<bool> computeGlue(const std::vector<Token>& tokens) {
  std::vector<bool> glue(tokens.size(), false);
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const Token& prev = tokens[i - 1];
    const Token& cur = tokens[i];

    if (isOpening(prev) || isClosing(cur)) {
      glue[i] = true;  // `(q_q)`, `{o1q o2q o3q}`: no padding inside the brackets
      continue;
    }
    // o#q: the read core is written against both of its ports, like a literal
    if ((prev.kind == Token::Kind::Operator && prev.text == "#") ||
        (cur.kind == Token::Kind::Operator && cur.text == "#")) {
      glue[i] = true;
      continue;
    }
    if (prev.kind == Token::Kind::Join || cur.kind == Token::Kind::Join) {
      glue[i] = true;
      continue;
    }
    // o5q, o"text"s: a literal core right after the void port
    if (prev.kind == Token::Kind::VoidPort &&
        (cur.kind == Token::Kind::IntLiteral || cur.kind == Token::Kind::StringLiteral)) {
      glue[i] = true;
      continue;
    }
    // o-3q: a negative literal
    if (prev.kind == Token::Kind::VoidPort && cur.kind == Token::Kind::Operator &&
        cur.text == "-") {
      glue[i] = true;
      continue;
    }
    // 5q, "text"s: the port that types a literal
    if ((prev.kind == Token::Kind::IntLiteral || prev.kind == Token::Kind::StringLiteral) &&
        isPort(cur)) {
      glue[i] = true;
      continue;
    }
    // -3: only when the sign was written against the number
    if (prev.kind == Token::Kind::Operator && prev.text == "-" &&
        cur.kind == Token::Kind::IntLiteral && touching(prev, cur)) {
      glue[i] = true;
      continue;
    }
    // o{o1q o2q o3q}[q]: an array literal core and the type that follows it.  The
    // `[]` index core is an ordinary core, so `[] [q]` keeps its spaces.
    if (prev.kind == Token::Kind::VoidPort && cur.kind == Token::Kind::LeftBrace) {
      glue[i] = true;
      continue;
    }
    if (prev.kind == Token::Kind::RightBrace && cur.kind == Token::Kind::LeftBracket) {
      glue[i] = true;
      continue;
    }
  }
  return glue;
}

std::vector<std::size_t> lineStartTable(const std::string& source) {
  std::vector<std::size_t> starts{0};
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\n') starts.push_back(i + 1);
  }
  return starts;
}

std::size_t lineOf(const std::vector<std::size_t>& starts, std::size_t position) {
  return static_cast<std::size_t>(
             std::upper_bound(starts.begin(), starts.end(), position) - starts.begin()) -
         1;
}

/// Rendered text of the code tokens in [begin, end), without indentation.
std::string flat(const std::vector<Token>& tokens, const std::vector<bool>& glue,
                 std::size_t begin, std::size_t end) {
  std::string out;
  bool first = true;
  for (std::size_t i = begin; i < end; ++i) {
    const Token& token = tokens[i];
    if (token.kind == Token::Kind::Comment) continue;
    if (!first && !glue[i]) out += ' ';
    out += token.spelling;
    first = false;
  }
  return out;
}

struct Line {
  std::size_t begin = 0;  ///< first token index, relative to the unit
  std::size_t end = 0;    ///< one past the last token index
  std::size_t indent = 0;
  bool comment = false;  ///< a comment on a line of its own
};

/// Splits `[begin, end)` into lines.
///
/// The layout is structural rather than width driven.  A chain of stages
/// (`~ ; @ ?`) with two operands, neither holding a chain of its own, is a
/// leaf and stays on one line; a longer chain, or one that contains another
/// chain, opens into a block: the brackets get lines of their own, one
/// operand per line, the connector at the end of the line it closes.  A
/// group that merely wraps an opened group -- an extra pair of parentheses,
/// or the two sides of a core -- opens with it but stays hugged against its
/// first and last line, so redundant brackets cost no lines and no indent.
/// Indentation is one tab per enclosing block, plus `baseIndent` for the
/// body as a whole; nothing is broken for width alone.
std::vector<Line> layoutBody(const std::vector<Token>& tokens,
                             const std::vector<std::size_t>& lineStarts, std::size_t begin,
                             std::size_t end, std::size_t baseIndent) {
  const std::size_t n = end - begin;
  std::vector<std::size_t> depth(n, 0);
  std::vector<bool> breaks(n, false);
  std::vector<bool> breaksAfter(n, false);

  std::size_t level = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Token& token = tokens[begin + i];
    if (isClosing(token)) --level;
    depth[i] = level;
    if (isOpening(token)) ++level;
    if (token.kind == Token::Kind::Comment) {
      const bool alone = (i == 0) || (lineOf(lineStarts, tokens[begin + i - 1].position) !=
                                      lineOf(lineStarts, token.position));
      if (alone) breaks[i] = true;
      breaksAfter[i] = true;
    }
  }

  // Match every opening bracket with its closing one, then decide bottom-up
  // which groups are vertical: scanning right to left means the groups nested
  // inside `i` have already been decided when `i` is reached.
  std::vector<std::size_t> match(n, n);
  {
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < n; ++i) {
      if (isOpening(tokens[begin + i])) {
        stack.push_back(i);
      } else if (isClosing(tokens[begin + i]) && !stack.empty()) {
        match[stack.back()] = i;
        match[i] = stack.back();
        stack.pop_back();
      }
    }
  }

  // Decide bottom-up which groups open up.  `chainDepth` counts the chains of
  // stages nested on the longest path below a group (itself included when a
  // stage sits at its own level), so a group opens on its own only when it
  // holds a chain within a chain; it also opens whenever a group inside it
  // already has, whatever connector joins them.  Scanning right to left means
  // the groups nested inside `i` have already been decided when `i` is reached.
  std::vector<std::size_t> chainDepth(n, 0);
  std::vector<std::size_t> ownStages(n, 0);
  std::vector<bool> ownChain(n, false);
  std::vector<bool> ownAppend(n, false);
  std::vector<bool> expanded(n, false);
  for (std::size_t i = n; i-- > 0;) {
    if (!isOpening(tokens[begin + i]) || match[i] == n) continue;
    const std::size_t inner = depth[i] + 1;
    std::size_t deepest = 0;
    bool childExpanded = false;
    for (std::size_t k = i + 1; k < match[i]; ++k) {
      if (depth[k] != inner) continue;
      if (isLayoutTrigger(tokens[begin + k])) ++ownStages[i];
      ownAppend[i] = ownAppend[i] || tokens[begin + k].kind == Token::Kind::Append;
      if (isOpening(tokens[begin + k])) {
        deepest = std::max(deepest, chainDepth[k]);
        childExpanded = childExpanded || expanded[k];
      }
    }
    ownChain[i] = ownStages[i] != 0;
    chainDepth[i] = deepest + (ownChain[i] ? 1 : 0);
    expanded[i] =
        childExpanded || (ownChain[i] && (chainDepth[i] >= 2 || ownStages[i] >= 2));
  }

  // An opened group with operands of its own is a block and earns bracket
  // lines and a level of indent; one without -- a wrapper around something
  // opened -- is hugged against the lines it wraps.
  std::vector<bool> block(n, false);
  for (std::size_t i = 0; i < n; ++i) {
    block[i] = expanded[i] && (ownChain[i] || ownAppend[i]);
  }

  // A block puts each of its operands on a line of its own, splitting at the
  // loosest connector it has: its stages when it is a chain, the `_` between
  // its faces when it wraps something opened.  The connector ends the line
  // it closes.  A hugged group adds no breaks of its own at all.
  for (std::size_t i = 0; i < n; ++i) {
    if (!expanded[i]) continue;
    const std::size_t close = match[i];
    const std::size_t inner = depth[i] + 1;
    if (block[i]) {
      if (i + 1 < n) breaks[i + 1] = true;
      breaks[close] = true;
    }
    for (std::size_t k = i + 1; k < close; ++k) {
      if (depth[k] != inner) continue;
      const Token& token = tokens[begin + k];
      const bool separator =
          ownChain[i] ? isLayoutTrigger(token) : token.kind == Token::Kind::Append;
      if (separator) breaks[k + 1] = true;
    }
  }

  // The body itself is an unbracketed group and follows the same rule: it
  // splits into one stage per line only when it holds a chain within a chain,
  // or when something inside it has already opened.
  {
    bool rootChain = false;
    bool rootExpanded = false;
    std::size_t rootStages = 0;
    std::size_t deepest = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (depth[i] != 0) continue;
      if (isLayoutTrigger(tokens[begin + i])) ++rootStages;
      rootChain = rootChain || isLayoutTrigger(tokens[begin + i]);
      if (isOpening(tokens[begin + i])) {
        deepest = std::max(deepest, chainDepth[i]);
        rootExpanded = rootExpanded || expanded[i];
      }
    }
    rootExpanded =
        rootExpanded || (rootChain && (deepest + 1 >= 2 || rootStages >= 2));
    if (rootExpanded) {
      for (std::size_t i = 0; i + 1 < n; ++i) {
        if (depth[i] != 0) continue;
        const Token& token = tokens[begin + i];
        const bool separator =
            rootChain ? isLayoutTrigger(token) : token.kind == Token::Kind::Append;
        if (separator) breaks[i + 1] = true;
      }
    }
  }

  // Indent counts enclosing blocks, not brackets: a hugged pair of
  // parentheses changes no line's depth, and a block's brackets sit at the
  // depth of the lines around them while its operands sit one tab deeper.
  std::vector<std::size_t> blockDepth(n, 0);
  {
    std::size_t depthNow = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (isClosing(tokens[begin + i]) && match[i] != n && block[match[i]] && depthNow != 0) {
        --depthNow;
      }
      blockDepth[i] = depthNow;
      if (isOpening(tokens[begin + i]) && block[i]) ++depthNow;
    }
  }

  std::vector<Line> lines;
  std::size_t i = 0;
  while (i < n) {
    const std::size_t start = i;
    bool hasCode = false;
    while (i < n) {
      if (i != start && breaks[i]) break;
      if (tokens[begin + i].kind != Token::Kind::Comment) hasCode = true;
      ++i;
      if (breaksAfter[i - 1]) break;
    }
    Line line;
    line.begin = start;
    line.end = i;
    line.comment = !hasCode;
    line.indent = baseIndent + blockDepth[start];
    lines.push_back(line);
  }
  return lines;
}

std::string renderLine(const std::vector<Token>& tokens, const std::vector<bool>& glue,
                       const Line& line, std::size_t begin) {
  std::string out(line.indent, '\t');
  if (line.comment) {
    out += tokens[begin + line.begin].spelling;
    return out;
  }
  for (std::size_t i = line.begin; i < line.end; ++i) {
    const Token& token = tokens[begin + i];
    if (token.kind == Token::Kind::Comment) {
      out += "  ";
      out += token.spelling;
      return out;
    }
    if (i != line.begin && !glue[begin + i]) out += ' ';
    out += token.spelling;
  }
  return out;
}

struct Range {
  std::size_t begin = 0;
  std::size_t end = 0;
};

}  // namespace

std::string format(const std::string& source) {
  std::vector<Token> tokens = tokenize(source, /*keepComments=*/true);
  if (tokens.empty()) return {};

  const Program program = parse(tokens);  // also validates the syntax
  // The end-of-input marker carries no text; layout works on the real tokens.
  tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                              [](const Token& token) { return token.kind == Token::Kind::End; }),
               tokens.end());
  const std::vector<bool> glue = computeGlue(tokens);
  const std::vector<std::size_t> lineStarts = lineStartTable(source);

  // A unit is one definition and the main expression; the parser records the
  // byte range of every definition, so the units follow the real syntax tree.
  std::vector<std::pair<std::size_t, std::size_t>> cuts;  // (name index, body end)
  for (const Definition& definition : program.definitions) {
    const std::size_t begin = static_cast<std::size_t>(
        std::lower_bound(tokens.begin(), tokens.end(), definition.position,
                         [](const Token& token, std::size_t position) {
                           return token.position < position;
                         }) -
        tokens.begin());
    std::size_t end = static_cast<std::size_t>(
        std::lower_bound(tokens.begin(), tokens.end(), definition.end,
                         [](const Token& token, std::size_t position) {
                           return token.position < position;
                         }) -
        tokens.begin());
    // A comment written after the last body token on the same line stays with
    // the definition it annotates.
    while (end < tokens.size() && tokens[end].kind == Token::Kind::Comment &&
           lineOf(lineStarts, tokens[end - 1].position) ==
               lineOf(lineStarts, tokens[end].position)) {
      ++end;
    }
    cuts.emplace_back(begin, end);
  }

  std::vector<Range> units;
  std::size_t cursor = 0;
  for (const auto& cut : cuts) {
    units.push_back(Range{cursor, cut.second});
    cursor = cut.second;
  }
  units.push_back(Range{cursor, tokens.size()});

  std::vector<std::string> out;
  for (const Range& range : units) {
    if (range.begin == range.end) continue;

    if (!out.empty()) out.push_back(std::string());

    // A comment block written above the unit stays at the left margin.
    std::size_t codeBegin = range.begin;
    while (codeBegin < range.end && tokens[codeBegin].kind == Token::Kind::Comment) {
      out.push_back(tokens[codeBegin].spelling);
      ++codeBegin;
    }
    // Comments on their own line after the last item belong to the file, not to
    // the expression: they go back to the left margin.
    std::size_t codeEnd = range.end;
    while (codeEnd > codeBegin && tokens[codeEnd - 1].kind == Token::Kind::Comment) --codeEnd;
    while (codeEnd < range.end && tokens[codeEnd].kind == Token::Kind::Comment &&
           lineOf(lineStarts, tokens[codeEnd - 1].position) ==
               lineOf(lineStarts, tokens[codeEnd].position)) {
      ++codeEnd;
    }
    const Range body{codeBegin, codeEnd};
    const Range tail{codeEnd, range.end};
    if (body.begin == body.end) {
      for (std::size_t i = tail.begin; i < tail.end; ++i) out.push_back(tokens[i].spelling);
      continue;
    }

    std::size_t fat = body.end;
    for (std::size_t i = body.begin; i < body.end; ++i) {
      if (tokens[i].kind == Token::Kind::FatArrow) {
        fat = i;
        break;
      }
    }

    const bool isDefinition = fat != body.end;
    const std::string whole = flat(tokens, glue, body.begin, body.end);

    bool commentsInside = false;
    for (std::size_t i = body.begin; i < body.end; ++i) {
      if (tokens[i].kind == Token::Kind::Comment) commentsInside = true;
    }

    if (!isDefinition) {
      for (const Line& line : layoutBody(tokens, lineStarts, body.begin, body.end, 0)) {
        out.push_back(renderLine(tokens, glue, line, body.begin));
      }
      for (std::size_t i = tail.begin; i < tail.end; ++i) out.push_back(tokens[i].spelling);
      continue;
    }

    const std::vector<Line> bodyLines =
        layoutBody(tokens, lineStarts, fat + 1, body.end, 1);

    // A definition whose body needs only one line keeps the header with it.
    if (!commentsInside && bodyLines.size() == 1) {
      out.push_back(whole);
      for (std::size_t i = tail.begin; i < tail.end; ++i) out.push_back(tokens[i].spelling);
      continue;
    }

    out.push_back(flat(tokens, glue, body.begin, fat + 1));
    for (const Line& line : bodyLines) {
      out.push_back(renderLine(tokens, glue, line, fat + 1));
    }
    for (std::size_t i = tail.begin; i < tail.end; ++i) out.push_back(tokens[i].spelling);
  }

  std::string result;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (i != 0) result += '\n';
    result += out[i];
  }
  if (!result.empty()) result += '\n';

  // Layout must never change meaning: the token stream has to survive the trip.
  // (Only the `_` gluing rule is whitespace sensitive, and it is the one rule
  // the spacing table is careful about.)  This is an internal check.
  std::vector<Token> again = tokenize(result);
  std::vector<Token> before;
  for (const Token& token : tokens) {
    if (token.kind != Token::Kind::Comment) before.push_back(token);
  }
  std::vector<std::pair<int, std::string>> a;
  for (const Token& token : before) {
    a.emplace_back(static_cast<int>(token.kind), token.text);
  }
  std::vector<std::pair<int, std::string>> b;
  for (const Token& token : again) {
    if (token.kind == Token::Kind::End) continue;
    b.emplace_back(static_cast<int>(token.kind), token.text);
  }
  if (a != b) {
    throw Error("internal error: formatting would change the token stream", 0);
  }

  return result;
}

}  // namespace qwq
