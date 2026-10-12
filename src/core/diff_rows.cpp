#include "core/diff_rows.h"

#include <algorithm>

namespace gk {

std::vector<DiffRow> alignedRows(const DiffHunk& hunk) {
    std::vector<DiffRow> rows;
    rows.reserve(hunk.lines.size());

    const std::vector<DiffLine>& lines = hunk.lines;
    std::size_t index = 0;
    while (index < lines.size()) {
        if (lines[index].kind == DiffLineKind::Context) {
            DiffRow row;
            row.left = static_cast<int>(index);
            row.right = static_cast<int>(index);
            rows.push_back(row);
            ++index;
            continue;
        }

        // A block of removals followed by a block of additions is one edit seen from
        // both sides. Pair them by position; the excess lines stand alone.
        const std::size_t removedStart = index;
        while (index < lines.size() && lines[index].kind == DiffLineKind::Removed) {
            ++index;
        }
        const std::size_t addedStart = index;
        while (index < lines.size() && lines[index].kind == DiffLineKind::Added) {
            ++index;
        }
        const std::size_t removedCount = addedStart - removedStart;
        const std::size_t addedCount = index - addedStart;
        const std::size_t paired = std::min(removedCount, addedCount);

        for (std::size_t i = 0; i < paired; ++i) {
            DiffRow row;
            row.left = static_cast<int>(removedStart + i);
            row.right = static_cast<int>(addedStart + i);
            rows.push_back(row);
        }
        for (std::size_t i = paired; i < removedCount; ++i) {
            DiffRow row;
            row.left = static_cast<int>(removedStart + i);
            rows.push_back(row);
        }
        for (std::size_t i = paired; i < addedCount; ++i) {
            DiffRow row;
            row.right = static_cast<int>(addedStart + i);
            rows.push_back(row);
        }
    }
    return rows;
}

} // namespace gk
