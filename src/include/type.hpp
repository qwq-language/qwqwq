#pragma once

#include <memory>
#include <string>
#include <vector>

namespace qwq {

struct Type;
using TypePtr = std::shared_ptr<const Type>;

/// A type is a *word*: an ordered sequence of atomic types.
///
///   o       the empty word
///   q       one Int
///   q_s_a   Int, Str and Bool, in that order
///
/// The `_` operator concatenates words and `o` is its unit, so
/// `o_q == q == q_o` and `(a_b)_c == a_(b_c)`.
using Word = std::vector<TypePtr>;

/// One atomic type.  Arrays are atomic: `[q]` occupies a single slot even
/// though its element type is a word.
struct Type {
  enum class Kind { Int, Str, Bool, Array };

  Kind kind;
  Word elements;  ///< element word; only meaningful for Kind::Array
};

TypePtr intType();
TypePtr strType();
TypePtr boolType();
TypePtr arrayType(Word elements);

bool equals(const TypePtr& a, const TypePtr& b);
bool equals(const Word& a, const Word& b);

/// Concatenates two words, i.e. the `_` operator.
Word concatenate(Word left, const Word& right);

/// Renders `q`, `[q]`, `q_s`, `o`, ...
std::string toString(const TypePtr& type);
std::string toString(const Word& word);

/// Render a word with explicit item boundaries, so a pack prints as the pack it
/// is: `{q,q,s,s}` with items `{2,2}` is `(q_q)_(s_s)`, and the same slots with
/// items `{1,1,1,1}` are `q_q_s_s`.  An empty `items` falls back to the flat
/// form.  Width-1 items stay bare, wider items get parentheses.
std::string toString(const Word& word, const std::vector<std::size_t>& items);

}  // namespace qwq
