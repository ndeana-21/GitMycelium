#include "model/word_diff.h"

#include <algorithm>
#include <vector>

namespace gk {
namespace {

/// Above this many tokens the quadratic table stops being worth its cost — a
/// minified file on one line is one token anyway — and everything counts as
/// changed, which is what the diff already showed.
constexpr std::size_t kMaxTokens = 400;

/// Above this many line-pairs the alignment table stops being worth its cost. A
/// conflict on a file this large is already a problem the reader has to solve
/// elsewhere; a second of layout to underline a few words is not the help they need.
constexpr std::size_t kMaxAlignCells = 4'000'000;

bool isWordCharacter(QChar c) {
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

bool isSpace(QChar c) {
    return c.isSpace();
}

std::vector<QString> tokenize(const QString& line) {
    std::vector<QString> tokens;
    int i = 0;
    const int size = line.size();
    while (i < size) {
        const QChar c = line.at(i);
        int end = i + 1;
        if (isWordCharacter(c)) {
            while (end < size && isWordCharacter(line.at(end))) {
                ++end;
            }
        } else if (isSpace(c)) {
            while (end < size && isSpace(line.at(end))) {
                ++end;
            }
        }
        tokens.push_back(line.mid(i, end - i));
        i = end;
    }
    return tokens;
}

} // namespace

bool WordDiff::whollyDifferent() const {
    if (before.empty() && after.empty()) {
        return false;
    }
    // Whitespace does not count as common ground: two lines that share only their
    // indentation have nothing in common worth not emphasising.
    const auto hasRealCommonGround = [](const std::vector<TextRun>& runs) {
        for (const TextRun& run : runs) {
            if (run.changed) {
                continue;
            }
            for (const QChar c : run.text) {
                if (!c.isSpace()) {
                    return true;
                }
            }
        }
        return false;
    };
    return !hasRealCommonGround(before) && !hasRealCommonGround(after);
}

WordDiff wordDiff(const QString& before, const QString& after) {
    WordDiff result;
    const std::vector<QString> left = tokenize(before);
    const std::vector<QString> right = tokenize(after);

    const auto pushPlain = [](std::vector<TextRun>& runs, const QString& text, bool changed) {
        // Runs that agree with their neighbour are one run: a hundred spans of one
        // character each is a hundred spans the painter has to deal with.
        if (!runs.empty() && runs.back().changed == changed) {
            runs.back().text += text;
            return;
        }
        runs.push_back(TextRun{text, changed});
    };

    if (left.empty() && right.empty()) {
        return result;
    }
    if (left.size() > kMaxTokens || right.size() > kMaxTokens) {
        for (const QString& token : left) {
            pushPlain(result.before, token, true);
        }
        for (const QString& token : right) {
            pushPlain(result.after, token, true);
        }
        return result;
    }

    // Longest common subsequence over tokens: everything it keeps is text that did
    // not move, everything else is what the change was.
    const std::size_t n = left.size();
    const std::size_t m = right.size();
    std::vector<std::vector<int>> lengths(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            lengths[i][j] = left[i - 1] == right[j - 1]
                                ? lengths[i - 1][j - 1] + 1
                                : std::max(lengths[i - 1][j], lengths[i][j - 1]);
        }
    }

    // Walk back from the corner, replaying the alignment forwards into the runs.
    std::vector<bool> leftKept(n, false);
    std::vector<bool> rightKept(m, false);
    std::size_t i = n;
    std::size_t j = m;
    while (i > 0 && j > 0) {
        if (left[i - 1] == right[j - 1]) {
            leftKept[i - 1] = true;
            rightKept[j - 1] = true;
            --i;
            --j;
        } else if (lengths[i - 1][j] >= lengths[i][j - 1]) {
            --i;
        } else {
            --j;
        }
    }

    result.before.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        pushPlain(result.before, left[k], !leftKept[k]);
    }
    for (std::size_t k = 0; k < m; ++k) {
        pushPlain(result.after, right[k], !rightKept[k]);
    }
    return result;
}

QVector<QPair<int, int>> emphasisRanges(const std::vector<TextRun>& runs) {
    QVector<QPair<int, int>> ranges;
    int at = 0;
    for (const TextRun& run : runs) {
        if (run.changed) {
            ranges.append({at, run.text.size()});
        }
        at += run.text.size();
    }
    return ranges;
}

std::vector<QVector<QPair<int, int>>> emphasisAgainstReference(
    const std::vector<QString>& side, const std::vector<QString>& reference) {
    std::vector<QVector<QPair<int, int>>> out(side.size());
    if (side.empty()) {
        return out;
    }

    // No reference at all: every line is new, so every non-blank line is emphasised
    // whole. This is the add/add shape, and inventing a base for it would hide it.
    if (reference.empty()) {
        for (std::size_t i = 0; i < side.size(); ++i) {
            if (!side[i].isEmpty()) {
                out[i].append({0, side[i].size()});
            }
        }
        return out;
    }

    const std::size_t n = side.size();
    const std::size_t m = reference.size();
    if (m > 0 && n > kMaxAlignCells / m) {
        return out;
    }

    // Line-level LCS: exact equality is the only thing that counts as "unchanged".
    std::vector<std::vector<int>> lengths(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            lengths[i][j] = side[i - 1] == reference[j - 1]
                                ? lengths[i - 1][j - 1] + 1
                                : std::max(lengths[i - 1][j], lengths[i][j - 1]);
        }
    }

    // The matched lines are the anchors; the lines between two anchors on either
    // side are one edited block.
    std::vector<std::pair<std::size_t, std::size_t>> matches;
    std::size_t i = n;
    std::size_t j = m;
    while (i > 0 && j > 0) {
        if (side[i - 1] == reference[j - 1]) {
            matches.push_back({i - 1, j - 1});
            --i;
            --j;
        } else if (lengths[i - 1][j] >= lengths[i][j - 1]) {
            --i;
        } else {
            --j;
        }
    }
    std::reverse(matches.begin(), matches.end());

    // A block's side lines pair positionally with its reference lines; the surplus
    // on the side is a pure insertion and is emphasised whole.
    std::size_t sideStart = 0;
    std::size_t refStart = 0;
    const auto handleBlock = [&](std::size_t sideEnd, std::size_t refEnd) {
        const std::size_t sideCount = sideEnd - sideStart;
        const std::size_t refCount = refEnd - refStart;
        for (std::size_t k = 0; k < sideCount; ++k) {
            const std::size_t index = sideStart + k;
            if (k < refCount) {
                out[index] = emphasisRanges(
                    wordDiff(reference[refStart + k], side[index]).after);
            } else if (!side[index].isEmpty()) {
                out[index].append({0, side[index].size()});
            }
        }
    };
    for (const std::pair<std::size_t, std::size_t>& anchor : matches) {
        handleBlock(anchor.first, anchor.second);
        sideStart = anchor.first + 1;
        refStart = anchor.second + 1;
    }
    handleBlock(n, m);
    return out;
}

} // namespace gk
