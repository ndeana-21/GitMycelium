#include "core/conflict_auto.h"

#include "core/conflict_regions.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace gk {
namespace {

/// Splits into lines, each keeping its own line ending. The text is written back
/// byte for byte where nothing was decided, so the line endings cannot be
/// normalised out from under the file.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start + 1));
        start = end + 1;
    }
    return lines;
}

std::string trimTrailingSpace(std::string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

/// A replacement of `base[baseStart, baseEnd)` with `lines`. An empty range means
/// lines were inserted; empty `lines` means lines were removed.
struct Change {
    std::size_t baseStart = 0;
    std::size_t baseEnd = 0;
    std::vector<std::string> lines;
};

/// Above this many lines the quadratic table stops being worth it, and a conflict
/// block that big wants a person anyway.
constexpr std::size_t kMaxMergeLines = 600;

/// The changes that turn `base` into `side`, in base order.
///
/// An LCS over lines: what both sides kept is the anchor, everything else is a
/// change. This is the same idea as the word diff, applied one level up.
bool changesFromBase(const std::vector<std::string>& base, const std::vector<std::string>& side,
                     std::vector<Change>* out) {
    const std::size_t n = base.size();
    const std::size_t m = side.size();
    if (n > kMaxMergeLines || m > kMaxMergeLines) {
        return false;
    }

    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            lcs[i][j] = base[i] == side[j] ? lcs[i + 1][j + 1] + 1
                                           : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }

    std::size_t i = 0;
    std::size_t j = 0;
    Change pending;
    bool open = false;
    const auto flush = [&]() {
        if (open) {
            // Where the change stopped in the base. Without this the change looks
            // like an insertion at its start, and the base lines it replaced are
            // then emitted a second time.
            pending.baseEnd = i;
            out->push_back(pending);
            pending = Change{};
            open = false;
        }
    };

    while (i < n && j < m) {
        if (base[i] == side[j]) {
            flush();
            ++i;
            ++j;
            continue;
        }
        if (!open) {
            pending.baseStart = i;
            open = true;
        }
        if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            ++i;
        } else {
            pending.lines.push_back(side[j]);
            ++j;
        }
    }
    if (!open) {
        pending.baseStart = i;
        open = true;
    }
    pending.baseEnd = n;
    for (; j < m; ++j) {
        pending.lines.push_back(side[j]);
    }
    flush();
    return true;
}

/// The lines of a side, one per line, for the trace.
std::string joinForTrace(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty()) {
            out += "|";
        }
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
            trimmed.pop_back();
        }
        out += trimmed;
    }
    return out;
}

/// True when the change takes base lines away and puts nothing in their place.
///
/// An empty replacement list is the tidy spelling of that, but the parse often produces a
/// single empty line instead — `\"\"` is a line like any other once the text has been split.
/// Checking only for an empty list missed exactly the case that brought dropped content
/// back, so what matters is whether the replacement carries any text at all.
bool changeRemovesLines(const Change& change) {
    if (change.baseStart >= change.baseEnd) {
        return false;
    }
    for (const std::string& line : change.lines) {
        if (!line.empty() && line != "\n" && line != "\r\n") {
            return false;
        }
    }
    return true;
}

/// True when two changes touch the same ground. An insertion is a point, a replacement
/// is a range: a point inside the other's range collides, a point exactly at its edge
/// does not — adjacent edits on neighbouring lines are not a disagreement, they are what
/// a three-way merge is for.
///
/// A deletion is the exception: deleting a line says "this does not belong here", and
/// inserting at either end of what it deleted says "something does". Merging those keeps
/// the deleted line's ground occupied and quietly brings the content back — the
/// duplicate-content shape, where each half looks right and the file is wrong. So an
/// insertion at the edge of a deletion collides, while at the edge of a replacement it
/// still does not.
bool changesCollide(const Change& a, const Change& b) {
    const bool aIsPoint = (a.baseStart == a.baseEnd);
    const bool bIsPoint = (b.baseStart == b.baseEnd);
    const bool aIsDeletion = changeRemovesLines(a);
    const bool bIsDeletion = changeRemovesLines(b);
    if (aIsPoint && bIsPoint) {
        return a.baseStart == b.baseStart;
    }
    if (aIsPoint) {
        return bIsDeletion ? (a.baseStart >= b.baseStart && a.baseStart <= b.baseEnd)
                           : (a.baseStart > b.baseStart && a.baseStart < b.baseEnd);
    }
    if (bIsPoint) {
        return aIsDeletion ? (b.baseStart >= a.baseStart && b.baseStart <= a.baseEnd)
                           : (b.baseStart > a.baseStart && b.baseStart < a.baseEnd);
    }
    return a.baseStart < b.baseEnd && b.baseStart < a.baseEnd;
}

/// Only called when the two sides' changes are known not to collide, so base order is
/// enough.
std::vector<std::string> applyBoth(const std::vector<std::string>& base,
                                   const std::vector<Change>& ours,
                                   const std::vector<Change>& theirs) {
    std::vector<std::string> out;
    std::size_t oi = 0;
    std::size_t ti = 0;
    std::size_t pos = 0;

    while (pos <= base.size()) {
        if (oi < ours.size() && ours[oi].baseStart == pos) {
            const Change& change = ours[oi++];
            out.insert(out.end(), change.lines.begin(), change.lines.end());
            pos = std::max(pos, change.baseEnd);
            continue;
        }
        if (ti < theirs.size() && theirs[ti].baseStart == pos) {
            const Change& change = theirs[ti++];
            out.insert(out.end(), change.lines.begin(), change.lines.end());
            pos = std::max(pos, change.baseEnd);
            continue;
        }
        if (pos == base.size()) {
            break;
        }
        out.push_back(base[pos]);
        ++pos;
    }
    // Anything left over (a change at the very end of the base).
    for (; oi < ours.size(); ++oi) {
        out.insert(out.end(), ours[oi].lines.begin(), ours[oi].lines.end());
    }
    for (; ti < theirs.size(); ++ti) {
        out.insert(out.end(), theirs[ti].lines.begin(), theirs[ti].lines.end());
    }
    return out;
}

/// A three-way merge of one conflict block, valid only when nothing is contested:
/// both sides changed the base, but not the same lines. This is precisely the case
/// git still reports as a conflict and a person resolves without thinking.
bool mergeDisjointChanges(const std::vector<std::string>& base,
                          const std::vector<std::string>& ours,
                          const std::vector<std::string>& theirs, std::vector<std::string>* out) {
    std::vector<Change> ourChanges;
    std::vector<Change> theirChanges;
    if (!changesFromBase(base, ours, &ourChanges) ||
        !changesFromBase(base, theirs, &theirChanges)) {
        return false;
    }
    // A change that replaces nothing with nothing is not a change. The extraction can
    // emit one where a side ran out of lines — the drop's shape, where our side has no
    // text at all — and a caller that merely counts changes then believes that side moved
    // when it did not, which is how a dropped line came back.
    const auto isReal = [](const Change& change) {
        return change.baseStart != change.baseEnd || !change.lines.empty();
    };
    std::erase_if(ourChanges, [&](const Change& change) { return !isReal(change); });
    std::erase_if(theirChanges, [&](const Change& change) { return !isReal(change); });

    // One side unchanged is handled by the cheaper rules; here both must have moved.
    if (ourChanges.empty() || theirChanges.empty()) {
        return false;
    }
    if (std::getenv("GK_TRACE_SWEEP")) {
        for (const char* side : {"ours", "theirs"}) {
            const std::vector<Change>& list = (side[0] == 'o') ? ourChanges : theirChanges;
            for (const Change& c : list) {
                std::fprintf(stderr, "CHANGE %s [%zu,%zu) lines=%zu deletion=%d\n", side, c.baseStart,
                             c.baseEnd, c.lines.size(), changeRemovesLines(c) ? 1 : 0);
            }
        }
    }
    for (const Change& mine : ourChanges) {
        for (const Change& other : theirChanges) {
            if (changesCollide(mine, other)) {
                return false;
            }
        }
    }
    *out = applyBoth(base, ourChanges, theirChanges);
    return true;
}

/// The same lines with trailing whitespace removed from each. Used only for the
/// last-resort comparison: two sides that differ in nothing but a trailing space
/// are not a disagreement worth a person's time.
bool equalIgnoringTrailingSpace(const std::vector<std::string>& a,
                                const std::vector<std::string>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (trimTrailingSpace(a[i]) != trimTrailingSpace(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

const char* toString(AutoReason reason) {
    switch (reason) {
    case AutoReason::Identical:
        return "both sides wrote the same thing";
    case AutoReason::OnlyTheirsChanged:
        return "our side is what the base had — took the incoming change";
    case AutoReason::OnlyOursChanged:
        return "the incoming side is what the base had — kept our change";
    case AutoReason::TrailingSpace:
        return "the sides differ only in trailing whitespace";
    case AutoReason::DisjointChanges:
        return "the sides changed different lines — merged both";
    case AutoReason::None:
        break;
    }
    return "both sides wrote different text";
}

AutoResolved autoResolveMarkedConflicts(const std::string& markedContent,
                                        const AutoOptions& options) {
    AutoResolved result;
    const std::vector<std::string> lines = splitLines(markedContent);

    std::size_t i = 0;
    while (i < lines.size()) {
        const std::size_t size = conflictOpeningSize(lines[i]);
        if (size == 0) {
            result.content += lines[i];
            ++i;
            continue;
        }
        const auto is = [&](std::size_t line, ConflictMarker kind) {
            return line < lines.size() && conflictMarkerOf(lines[line], size) == kind;
        };

        const std::size_t blockStart = i;
        std::vector<std::string> ours;
        std::vector<std::string> base;
        std::vector<std::string> theirs;
        bool hasBase = false;

        ++i;
        while (i < lines.size() && !is(i, ConflictMarker::Base) && !is(i, ConflictMarker::Split)) {
            ours.push_back(lines[i]);
            ++i;
        }
        if (is(i, ConflictMarker::Base)) {
            hasBase = true;
            ++i;
            while (i < lines.size() && !is(i, ConflictMarker::Split)) {
                base.push_back(lines[i]);
                ++i;
            }
        }
        if (is(i, ConflictMarker::Split)) {
            ++i;
            while (i < lines.size() && !is(i, ConflictMarker::Theirs)) {
                theirs.push_back(lines[i]);
                ++i;
            }
        }

        bool closed = false;
        if (is(i, ConflictMarker::Theirs)) {
            closed = true;
            ++i;
        }

        AutoHunk hunk;
        if (!closed) {
            // Unbalanced markers. Putting the original bytes back is the only
            // honest answer: guessing where the conflict ended would corrupt a file
            // that is already broken.
            for (std::size_t line = blockStart; line < lines.size(); ++line) {
                result.content += lines[line];
            }
            hunk.reason = AutoReason::None;
            result.hunks.push_back(hunk);
            ++result.leftCount;
            break;
        }

        std::vector<std::string> merged;
        const std::vector<std::string>* chosen = &ours;
        if (ours == theirs) {
            hunk.reason = AutoReason::Identical;
        } else if (hasBase && theirs == base) {
            // The replayed change is empty here whatever the operation: keeping our side
            // is the same as applying nothing.
            hunk.reason = AutoReason::OnlyOursChanged;
        } else if (hasBase && ours == base && options.rebaseStep) {
            // A rebase step whose our-side is exactly the base: their side is then the
            // snapshot of the commit being replayed, context and all. Taking it whole
            // puts back lines from commits the reader removed on purpose, and the
            // disjoint rule below would do the same thing by another name — one side has
            // no changes, so merging both sides is just taking theirs. Neither is
            // settled here.
            hunk.reason = AutoReason::None;
        } else if (hasBase && ours == base) {
            hunk.reason = AutoReason::OnlyTheirsChanged;
            chosen = &theirs;
        } else if (equalIgnoringTrailingSpace(ours, theirs)) {
            hunk.reason = AutoReason::TrailingSpace;
        } else if (hasBase && mergeDisjointChanges(base, ours, theirs, &merged)) {
            // Both sides did something, just not to the same lines. Git reports this
            // as a conflict because its hunks are wider than the changes in them;
            // there is nothing here to decide.
            hunk.reason = AutoReason::DisjointChanges;
        }

        if (std::getenv("GK_TRACE_SWEEP")) {
            std::fprintf(stderr,
                         "HUNK reason=%s hasBase=%d rebase=%d ours=[%s] base=[%s] theirs=[%s]\n",
                         toString(hunk.reason), hasBase ? 1 : 0, options.rebaseStep ? 1 : 0,
                         joinForTrace(ours).c_str(), joinForTrace(base).c_str(),
                         joinForTrace(theirs).c_str());
        }
        if (hunk.reason == AutoReason::None) {
            // Left exactly as it was. A file that quietly lost its conflict markers
            // is a file nobody knows to look at.
            for (std::size_t line = blockStart; line < i; ++line) {
                result.content += lines[line];
            }
            ++result.leftCount;
        } else {
            const std::vector<std::string>& picked =
                hunk.reason == AutoReason::DisjointChanges ? merged : *chosen;
            for (const std::string& line : picked) {
                hunk.text += line;
                result.content += line;
            }
            hunk.resolved = true;
            ++result.resolvedCount;
        }
        result.hunks.push_back(hunk);
    }

    return result;
}

bool hasConflictMarkers(const std::string& content) {
    // Any length of marker counts: the editor may have drawn the block with longer
    // markers than git's seven, and an unresolved region keeps them. A lone `=======`
    // is left out — a seven-character heading underline is text — but no block can be
    // left behind without its `<<<<<<<`, `|||||||` or `>>>>>>>` too.
    std::size_t start = 0;
    while (start < content.size()) {
        const std::size_t end = content.find('\n', start);
        const std::string_view line =
            std::string_view(content).substr(start, end == std::string::npos ? end : end - start);
        if (!line.empty() && (line[0] == '<' || line[0] == '|' || line[0] == '>')) {
            std::size_t run = 0;
            while (run < line.size() && line[run] == line[0]) {
                ++run;
            }
            if (run >= kDefaultConflictMarkerSize &&
                conflictMarkerOf(line, run) != ConflictMarker::None) {
                return true;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

} // namespace gk
