#include "core/patch_selection.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace gk {
namespace {

bool isNewFile(const FileDiff& diff) {
    return diff.kind == ChangeKind::Added || diff.kind == ChangeKind::Untracked;
}

bool isDeletedFile(const FileDiff& diff) {
    return diff.kind == ChangeKind::Deleted;
}

std::string octalMode(int mode) {
    if (mode <= 0) {
        return std::string("100644");
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%o", mode);
    return std::string(buf);
}

/// One output line of a hunk, oriented so "Removed" always means the old side of
/// the patch being written. Reversing swaps the kind and the two line numbers.
struct Piece {
    DiffLineKind kind = DiffLineKind::Context;
    const DiffLine* line = nullptr;
    int oldLine = 0;
    int newLine = 0;

    bool onOld() const { return kind != DiffLineKind::Added; }
    bool onNew() const { return kind != DiffLineKind::Removed; }
};

/// The lines this patch carries for one hunk, in output orientation. A changed
/// line that is not selected is left out entirely; context always stays.
std::vector<Piece> gatherPieces(const DiffHunk& hunk, bool wholeHunk,
                                const std::vector<std::size_t>& selected, bool reverse) {
    const auto wanted = [&](std::size_t index) {
        return wholeHunk ||
               std::find(selected.begin(), selected.end(), index) != selected.end();
    };

    std::vector<Piece> pieces;
    for (std::size_t i = 0; i < hunk.lines.size(); ++i) {
        const DiffLine& line = hunk.lines[i];
        if (line.kind != DiffLineKind::Context && !wanted(i)) {
            continue;
        }
        Piece piece;
        piece.line = &line;
        if (reverse) {
            piece.kind = line.kind == DiffLineKind::Removed   ? DiffLineKind::Added
                         : line.kind == DiffLineKind::Added   ? DiffLineKind::Removed
                                                              : DiffLineKind::Context;
            piece.oldLine = line.newLine;
            piece.newLine = line.oldLine;
        } else {
            piece.kind = line.kind;
            piece.oldLine = line.oldLine;
            piece.newLine = line.newLine;
        }
        pieces.push_back(piece);
    }
    return pieces;
}

struct Group {
    std::vector<Piece> pieces;
};

/// Splits the kept lines into hunks that each stay internally consistent: within
/// a hunk the old-side numbers are consecutive and so are the new-side ones. A
/// dropped change in the middle breaks that, so a new hunk starts there.
std::vector<Group> splitIntoGroups(const std::vector<Piece>& pieces) {
    std::vector<Group> groups;
    Group current;
    bool haveOld = false;
    bool haveNew = false;
    int lastOld = 0;
    int lastNew = 0;
    for (const Piece& piece : pieces) {
        bool continues = true;
        if (!current.pieces.empty()) {
            if (piece.onOld() && haveOld) {
                continues = continues && piece.oldLine == lastOld + 1;
            }
            if (piece.onNew() && haveNew) {
                continues = continues && piece.newLine == lastNew + 1;
            }
        }
        if (!continues) {
            groups.push_back(current);
            current = Group{};
            haveOld = false;
            haveNew = false;
        }
        current.pieces.push_back(piece);
        if (piece.onOld()) {
            lastOld = piece.oldLine;
            haveOld = true;
        }
        if (piece.onNew()) {
            lastNew = piece.newLine;
            haveNew = true;
        }
    }
    if (!current.pieces.empty()) {
        groups.push_back(current);
    }
    return groups;
}

std::string range(int start, int count) {
    return std::to_string(start) + "," + std::to_string(count);
}

std::string emitGroup(const Group& group, int oldStart, int newStart) {
    int oldLines = 0;
    int newLines = 0;
    for (const Piece& piece : group.pieces) {
        if (piece.onOld()) {
            ++oldLines;
        }
        if (piece.onNew()) {
            ++newLines;
        }
    }

    std::string out = "@@ -" + range(oldStart, oldLines) + " +" + range(newStart, newLines) +
                      " @@\n";
    for (const Piece& piece : group.pieces) {
        const char sign = piece.kind == DiffLineKind::Context ? ' '
                          : piece.kind == DiffLineKind::Added ? '+'
                                                              : '-';
        out += sign;
        out += piece.line->text;
        out += '\n';
        if (piece.line->noNewline) {
            out += "\\ No newline at end of file\n";
        }
    }
    return out;
}

} // namespace

std::string renderPatch(const FileDiff& diff, const std::vector<StageSelection>& selections,
                        bool reverse) {
    std::vector<bool> chosen(diff.hunks.size(), false);
    std::vector<std::vector<std::size_t>> linesFor(diff.hunks.size());
    for (const StageSelection& selection : selections) {
        if (selection.hunk >= diff.hunks.size()) {
            throw std::runtime_error("no such hunk in this diff");
        }
        chosen[selection.hunk] = true;
        for (const std::size_t index : selection.lines) {
            if (index >= diff.hunks[selection.hunk].lines.size()) {
                throw std::runtime_error("no such line in this hunk");
            }
        }
        linesFor[selection.hunk] = selection.lines;
    }

    const std::string oldPath = diff.oldPath.empty() ? diff.newPath : diff.oldPath;
    const std::string newPath = diff.newPath.empty() ? diff.oldPath : diff.newPath;
    const std::string aPath = reverse ? newPath : oldPath;
    const std::string bPath = reverse ? oldPath : newPath;

    // Which side exists. A reverse patch turns a creation into a deletion and the
    // other way round, and `/dev/null` follows.
    const bool oldExists = reverse ? !isDeletedFile(diff) : !isNewFile(diff);
    const bool newExists = reverse ? !isNewFile(diff) : !isDeletedFile(diff);
    const int aMode = reverse ? diff.newMode : diff.oldMode;
    const int bMode = reverse ? diff.oldMode : diff.newMode;

    std::string out = "diff --git a/" + aPath + " b/" + bPath + "\n";
    if (!oldExists) {
        out += "new file mode " + octalMode(bMode) + "\n";
    } else if (!newExists) {
        out += "deleted file mode " + octalMode(aMode) + "\n";
    }
    out += oldExists ? "--- a/" + aPath + "\n" : "--- /dev/null\n";
    out += newExists ? "+++ b/" + bPath + "\n" : "+++ /dev/null\n";

    for (std::size_t hi = 0; hi < diff.hunks.size(); ++hi) {
        if (!chosen[hi]) {
            continue;
        }
        const DiffHunk& hunk = diff.hunks[hi];
        const bool wholeHunk = linesFor[hi].empty();
        const std::vector<Piece> pieces = gatherPieces(hunk, wholeHunk, linesFor[hi], reverse);
        if (pieces.empty()) {
            continue;
        }
        // A pure insertion or deletion has no line on one side; git names that side
        // by the line the change sits after, which is what `next*` tracks. Each hunk
        // seeds it from its own header, so a split group in a later hunk is not
        // measured against the start of the first one.
        int nextOld = reverse ? hunk.newStart : hunk.oldStart;
        int nextNew = reverse ? hunk.oldStart : hunk.newStart;
        const std::vector<Group> groups = splitIntoGroups(pieces);
        for (const Group& group : groups) {
            int firstOld = 0;
            int firstNew = 0;
            bool gotOld = false;
            bool gotNew = false;
            for (const Piece& piece : group.pieces) {
                if (piece.onOld() && !gotOld) {
                    firstOld = piece.oldLine;
                    gotOld = true;
                }
                if (piece.onNew() && !gotNew) {
                    firstNew = piece.newLine;
                    gotNew = true;
                }
            }
            const int oldStart = gotOld ? firstOld : std::max(0, nextOld - 1);
            const int newStart = gotNew ? firstNew : std::max(0, nextNew - 1);
            out += emitGroup(group, oldStart, newStart);

            int oldLines = 0;
            int newLines = 0;
            for (const Piece& piece : group.pieces) {
                oldLines += piece.onOld() ? 1 : 0;
                newLines += piece.onNew() ? 1 : 0;
            }
            if (oldLines > 0) {
                nextOld = oldStart + oldLines;
            }
            if (newLines > 0) {
                nextNew = newStart + newLines;
            }
        }
    }
    return out;
}

} // namespace gk
