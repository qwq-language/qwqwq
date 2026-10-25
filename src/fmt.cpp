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

/// `~ @ ?` -- the connectors that hold a chain of stages together.
/// `:` and `_` are not composition stages; `_` is handled separately as a
/// Pack/append separator by the layout code below.
bool isLayoutTrigger(const Token& token) {
  switch (token.kind) {
    case Token::Kind::Compose:
    case Token::Kind::Loop:
    case Token::Kind::Branch: return true;
    default: return false;
  }
}

bool touching(const Token& left, const Token& right) {
  return left.position + left.spelling.size() == right.position;
}

/// A newline may continue a template argument list at a separator or a
/// delimiter. Otherwise it is a declaration/expression boundary, so do not
/// pair a comparison core with a `>` from a later line.
bool continuesTemplateAcrossLine(const Token& previous, const Token& current) {
  if (previous.kind == Token::Kind::Comment ||
      current.kind == Token::Kind::Comment) {
    return false;
  }
  if (previous.kind == Token::Kind::Comma ||
      (previous.kind == Token::Kind::Operator && previous.text == "<")) {
    return true;
  }
  if (previous.kind == Token::Kind::LeftParen ||
      previous.kind == Token::Kind::LeftBracket ||
      previous.kind == Token::Kind::LeftBrace) {
    return true;
  }
  if (current.kind == Token::Kind::Comma ||
      current.kind == Token::Kind::RightParen ||
      current.kind == Token::Kind::RightBracket ||
      current.kind == Token::Kind::RightBrace) {
    return true;
  }
  return current.kind == Token::Kind::Operator &&
         (current.text == ">" ||
          (current.text == "<" && previous.kind == Token::Kind::Identifier));
}

/// List commas are printed without a preceding space and with one following
/// space. Underscores are glued to both adjacent tokens in every context; the
/// parser, not whitespace, determines whether one joins a Pack or appends.
std::vector<bool> computeGlue(const std::vector<Token>& tokens,
                              const std::vector<bool>& hashReshapes) {
  std::vector<bool> glue(tokens.size(), false);
  std::vector<bool> genericGlue(tokens.size(), false);
  const auto markTemplate = [&](std::size_t open) {
    if (open + 1 >= tokens.size()) return;
    int angleDepth = 1;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    for (std::size_t j = open + 1; j < tokens.size(); ++j) {
      if (tokens[j].lineBreakBefore &&
          !continuesTemplateAcrossLine(tokens[j - 1], tokens[j])) {
        break;
      }
      if (tokens[j].kind == Token::Kind::Comment) break;
      if (tokens[j].kind == Token::Kind::LeftParen) {
        ++parenDepth;
        continue;
      }
      if (tokens[j].kind == Token::Kind::RightParen) {
        if (parenDepth > 0) --parenDepth;
        continue;
      }
      if (tokens[j].kind == Token::Kind::LeftBracket) {
        ++bracketDepth;
        continue;
      }
      if (tokens[j].kind == Token::Kind::RightBracket) {
        if (bracketDepth > 0) --bracketDepth;
        continue;
      }
      if (tokens[j].kind == Token::Kind::LeftBrace) {
        ++braceDepth;
        continue;
      }
      if (tokens[j].kind == Token::Kind::RightBrace) {
        if (braceDepth > 0) --braceDepth;
        continue;
      }
      if (parenDepth != 0 || bracketDepth != 0 || braceDepth != 0 ||
          tokens[j].kind != Token::Kind::Operator) {
        continue;
      }
      if (tokens[j].text == "<") {
        ++angleDepth;
      } else if (tokens[j].text == ">" && --angleDepth == 0) {
        genericGlue[open] = true;
        genericGlue[open + 1] = true;
        genericGlue[j] = true;
        break;
      }
    }
  };
  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (tokens[i].kind == Token::Kind::Identifier &&
        tokens[i + 1].kind == Token::Kind::Operator &&
        tokens[i + 1].text == "<") {
      markTemplate(i + 1);
    }
    if (i + 4 < tokens.size() &&
        tokens[i].kind == Token::Kind::Identifier &&
        tokens[i].text == "operator" &&
        tokens[i + 1].kind == Token::Kind::LeftParen &&
        tokens[i + 2].kind == Token::Kind::Operator &&
        tokens[i + 3].kind == Token::Kind::RightParen) {
      glue[i + 1] = true;
      glue[i + 2] = true;
      glue[i + 3] = true;
      if (tokens[i + 4].kind == Token::Kind::Operator &&
          tokens[i + 4].text == "<") {
        markTemplate(i + 4);
      }
    }
    if (i + 5 < tokens.size() &&
        tokens[i].kind == Token::Kind::Identifier &&
        tokens[i].text == "operator" &&
        tokens[i + 1].kind == Token::Kind::LeftParen &&
        tokens[i + 2].kind == Token::Kind::LeftBracket &&
        tokens[i + 3].kind == Token::Kind::RightBracket &&
        tokens[i + 4].kind == Token::Kind::RightParen) {
      glue[i + 1] = true;
      glue[i + 2] = true;
      glue[i + 3] = true;
      glue[i + 4] = true;
      if (tokens[i + 5].kind == Token::Kind::Operator &&
          tokens[i + 5].text == "<") {
        markTemplate(i + 5);
      }
    }
  }

  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const Token& prev = tokens[i - 1];
    const Token& cur = tokens[i];

    if (genericGlue[i]) {
      glue[i] = true;
      continue;
    }
    if (prev.kind == Token::Kind::Underscore ||
        cur.kind == Token::Kind::Underscore) {
      glue[i] = true;
      continue;
    }
    if (isOpening(prev) || isClosing(cur)) {
      glue[i] = true;
      continue;
    }
    if (cur.kind == Token::Kind::Comma) {
      glue[i] = true;
      continue;
    }
    // Keep a negative number visually attached to its sign.
    if (prev.kind == Token::Kind::Operator && prev.text == "-" &&
        cur.kind == Token::Kind::IntLiteral && touching(prev, cur)) {
      glue[i] = true;
      continue;
    }
    // An array literal/constructor core is attached to its input port; `[]`
    // remains an ordinary operator and is separated from the type after it.
    if ((isPort(prev) || prev.kind == Token::Kind::RightBracket ||
         prev.kind == Token::Kind::RightParen) &&
        cur.kind == Token::Kind::LeftBrace) {
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

void collectHashReshapePositions(const Expr& expr,
                                 std::vector<std::size_t>& positions);

void collectHashReshapePositions(const Port& port,
                                 std::vector<std::size_t>& positions);

bool syntacticallyEmpty(const Port& port) {
  for (const PortPart& part : port.parts) {
    if (part.kind == PortPart::Kind::Primitive) {
      if (!part.type.empty()) return false;
    } else if (part.kind == PortPart::Kind::Group) {
      if (!syntacticallyEmpty(*part.inner)) return false;
    } else {
      return false;
    }
  }
  return true;
}

void collectHashReshapePositions(const Port& port,
                                 std::vector<std::size_t>& positions) {
  for (const PortPart& part : port.parts) {
    if (part.inner) collectHashReshapePositions(*part.inner, positions);
    if (part.expression) collectHashReshapePositions(*part.expression, positions);
    for (const std::unique_ptr<Port>& argument : part.arguments) {
      if (argument) collectHashReshapePositions(*argument, positions);
    }
  }
}

void collectHashReshapePositions(const Expr& expr,
                                 std::vector<std::size_t>& positions) {
  if (expr.kind == Expr::Kind::Face && expr.core == Core::Hash &&
      expr.hasDomain && !syntacticallyEmpty(expr.domain)) {
    positions.push_back(expr.corePosition);
  }
  if (expr.hasDomain) collectHashReshapePositions(expr.domain, positions);
  if (expr.hasCodomain) collectHashReshapePositions(expr.codomain, positions);
  for (const Port& argument : expr.templateArguments) {
    collectHashReshapePositions(argument, positions);
  }
  for (const ExprPtr& element : expr.elements) {
    if (element) collectHashReshapePositions(*element, positions);
  }
  if (expr.left) collectHashReshapePositions(*expr.left, positions);
  if (expr.right) collectHashReshapePositions(*expr.right, positions);
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
  std::vector<bool> ownUnderscore(n, false);
  std::vector<bool> expanded(n, false);
  for (std::size_t i = n; i-- > 0;) {
    if (!isOpening(tokens[begin + i]) || match[i] == n) continue;
    const std::size_t inner = depth[i] + 1;
    std::size_t deepest = 0;
    bool childExpanded = false;
    for (std::size_t k = i + 1; k < match[i]; ++k) {
      if (depth[k] != inner) continue;
      if (isLayoutTrigger(tokens[begin + k])) ++ownStages[i];
      ownUnderscore[i] =
          ownUnderscore[i] || tokens[begin + k].kind == Token::Kind::Underscore;
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
    block[i] = expanded[i] && (ownChain[i] || ownUnderscore[i]);
  }

  // A block puts each of its operands on a line of its own, splitting at the
  // loosest connector it has: its stages when it is a chain, underscores when
  // it wraps something opened. The connector ends the line it closes. A hugged
  // group adds no breaks of its own at all.
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
          ownChain[i] ? isLayoutTrigger(token) : token.kind == Token::Kind::Underscore;
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
            rootChain ? isLayoutTrigger(token) : token.kind == Token::Kind::Underscore;
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

std::string format(const std::string& source, bool allowDeclarations) {
  std::vector<Token> tokens = tokenize(source, /*keepComments=*/true);
  if (tokens.empty()) return {};

  const Program program = parse(tokens, allowDeclarations);  // also validates
  // The end-of-input marker carries no text; layout works on the real tokens.
  tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                              [](const Token& token) { return token.kind == Token::Kind::End; }),
               tokens.end());

  std::vector<std::size_t> hashReshapePositions;
  for (const TypeAliasDefinition& alias : program.typeAliases) {
    collectHashReshapePositions(alias.body, hashReshapePositions);
  }
  for (const Definition& definition : program.definitions) {
    collectHashReshapePositions(definition.domain, hashReshapePositions);
    collectHashReshapePositions(definition.codomain, hashReshapePositions);
    if (definition.body) {
      collectHashReshapePositions(*definition.body, hashReshapePositions);
    }
  }
  if (program.main) collectHashReshapePositions(*program.main, hashReshapePositions);

  std::vector<bool> hashReshapes(tokens.size(), false);
  for (const std::size_t position : hashReshapePositions) {
    const auto token = std::lower_bound(
        tokens.begin(), tokens.end(), position,
        [](const Token& item, std::size_t offset) { return item.position < offset; });
    if (token != tokens.end() && token->position == position &&
        token->kind == Token::Kind::Operator && token->text == "#") {
      hashReshapes[static_cast<std::size_t>(token - tokens.begin())] = true;
    }
  }
  std::vector<bool> glue = computeGlue(tokens, hashReshapes);
  for (const Definition& definition : program.definitions) {
    if (!definition.hasSignature) continue;
    const auto name = std::lower_bound(
        tokens.begin(), tokens.end(), definition.position,
        [](const Token& token, std::size_t position) {
          return token.position < position;
        });
    if (name == tokens.end() || name->position != definition.position) continue;

    const std::size_t nameIndex = static_cast<std::size_t>(name - tokens.begin());
    std::size_t signatureIndex = nameIndex + 1;
    if (name->text == "operator" && signatureIndex + 2 < tokens.size() &&
        tokens[signatureIndex].kind == Token::Kind::LeftParen) {
      if (tokens[signatureIndex + 1].kind == Token::Kind::Operator &&
          tokens[signatureIndex + 2].kind == Token::Kind::RightParen) {
        signatureIndex += 3;
      } else if (signatureIndex + 3 < tokens.size() &&
                 tokens[signatureIndex + 1].kind == Token::Kind::LeftBracket &&
                 tokens[signatureIndex + 2].kind == Token::Kind::RightBracket &&
                 tokens[signatureIndex + 3].kind == Token::Kind::RightParen) {
        signatureIndex += 4;
      }
    }
    if (signatureIndex < tokens.size() &&
        tokens[signatureIndex].kind == Token::Kind::Operator &&
        tokens[signatureIndex].text == "<") {
      ++signatureIndex;
      while (signatureIndex < tokens.size() &&
             !(tokens[signatureIndex].kind == Token::Kind::Operator &&
               tokens[signatureIndex].text == ">")) {
        ++signatureIndex;
      }
      if (signatureIndex == tokens.size()) continue;
      ++signatureIndex;
    }
    if (signatureIndex < tokens.size() &&
        tokens[signatureIndex].kind == Token::Kind::LeftParen) {
      glue[signatureIndex] = true;
    }
  }
  const std::vector<std::size_t> lineStarts = lineStartTable(source);

  // `=> header.awa` keeps its own line and its own spelling: the path is not a
  // division or a swap, so it must not be laid out as an expression.
  std::vector<std::pair<std::size_t, std::size_t>> includeRanges;  // [begin, end)
  const auto endsWithAwa = [&](std::size_t begin, std::size_t end) {
    std::string path;
    for (std::size_t i = begin; i < end; ++i) path += tokens[i].spelling;
    return path.size() > 4 && path.compare(path.size() - 4, 4, ".awa") == 0;
  };
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind != Token::Kind::FatArrow) continue;
    if (i != 0 && !tokens[i].lineBreakBefore) continue;
    std::size_t end = i + 1;
    while (end < tokens.size() && !tokens[end].lineBreakBefore &&
           tokens[end].kind != Token::Kind::Comment) {
      ++end;
    }
    if (end == i + 1 || !endsWithAwa(i + 1, end)) continue;
    includeRanges.emplace_back(i, end);
    i = end - 1;
  }

  // A unit is one class declaration, function definition, or the main expression;
  // source ranges keep declarations in their written order.
  std::vector<std::pair<std::size_t, std::size_t>> cuts;  // (name index, body end)
  const auto addCut = [&](std::size_t position, std::size_t endPosition) {
    const std::size_t begin = static_cast<std::size_t>(
        std::lower_bound(tokens.begin(), tokens.end(), position,
                         [](const Token& token, std::size_t offset) {
                           return token.position < offset;
                         }) -
        tokens.begin());
    std::size_t end = static_cast<std::size_t>(
        std::lower_bound(tokens.begin(), tokens.end(), endPosition,
                         [](const Token& token, std::size_t offset) {
                           return token.position < offset;
                         }) -
        tokens.begin());
    // A comment written after the last body token on the same line stays with
    // the declaration it annotates.
    while (end < tokens.size() && end > 0 &&
           tokens[end].kind == Token::Kind::Comment &&
           lineOf(lineStarts, tokens[end - 1].position) ==
               lineOf(lineStarts, tokens[end].position)) {
      ++end;
    }
    cuts.emplace_back(begin, end);
  };
  for (const TypeAliasDefinition& alias : program.typeAliases) {
    addCut(alias.position, alias.end);
  }
  for (const Definition& definition : program.definitions) {
    addCut(definition.position, definition.end);
  }
  for (const auto& range : includeRanges) {
    cuts.emplace_back(range.first, range.second);
  }
  std::sort(cuts.begin(), cuts.end());

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

    const auto include = std::find_if(
        includeRanges.begin(), includeRanges.end(),
        [&](const std::pair<std::size_t, std::size_t>& range2) {
          return range2.first == range.begin && range2.second == range.end;
        });
    if (include != includeRanges.end()) {
      if (!out.empty()) out.push_back(std::string());
      std::string line = "=> ";
      for (std::size_t i = range.begin + 1; i < range.end; ++i) {
        line += tokens[i].spelling;
      }
      out.push_back(line);
      continue;
    }

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
