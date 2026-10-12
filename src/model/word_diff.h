#pragma once

#include <QPair>
#include <QString>
#include <QVector>

#include <vector>

namespace gk {

struct TextRun {
    QString text;
    bool changed = false;
};

struct WordDiff {
    std::vector<TextRun> before;
    std::vector<TextRun> after;

    /// True when nothing matched, which is what a completely rewritten line looks
    /// like and what the renderer skips emphasis for.
    bool whollyDifferent() const;
};

/// Compares two lines word by word.
///
/// A modified line is shown as one removed line and one added line, which hides
/// what actually moved: renaming one variable in a long expression reads as two
/// entirely different lines. Comparing tokens and marking the ones that did not
/// survive makes the pair readable at a glance, which is the whole point.
///
/// Tokens are runs of word characters, runs of whitespace, and single other
/// characters — so `foo(x)` against `foo(y)` differs in one letter and not in the
/// whole line, while `f(a)` against `g(b)` differs in three places and not one.
WordDiff wordDiff(const QString& before, const QString& after);

/// The changed stretches of a word diff, as offsets into the line. The runs are the
/// line cut into consecutive pieces, so the offsets accumulate. Each pair is
/// `(offset, length)`.
QVector<QPair<int, int>> emphasisRanges(const std::vector<TextRun>& runs);

/// Per-line emphasis for `side` measured against `reference`: for every line of
/// `side`, the ranges whose words the reference does not share. Indexed by line, so
/// an unchanged line is empty.
///
/// The lines are aligned first — a single line-level LCS — because `wordDiff` works
/// on a pair and a conflict hands over blocks. Within a block of changed lines the
/// two sides are paired positionally and diffed word by word; a side line with no
/// counterpart is emphasised whole. When the reference is empty every line is new,
/// and when either side is too large to align the result is empty too — the caller's
/// signal to draw plain text rather than spend a quadratic table on it.
std::vector<QVector<QPair<int, int>>> emphasisAgainstReference(
    const std::vector<QString>& side, const std::vector<QString>& reference);

} // namespace gk
