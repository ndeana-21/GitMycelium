#pragma once

#include "core/types.h"

#include <vector>

namespace gk {

/// One visual row of a side-by-side diff.
///
/// `left` and `right` are indices into the hunk's `lines`, or -1 when that side has
/// nothing at this row — the cell is drawn empty so the rows below stay aligned with
/// their counterparts.
struct DiffRow {
    int left = -1;
    int right = -1;
};

/// Aligns one hunk's lines for a side-by-side view.
///
/// Context sits on both sides of one row. Within a change block the i-th removal pairs
/// with the i-th addition — the same rule the unified view uses to decide which lines'
/// words to compare — and those two go on one row so a change reads across. Whatever is
/// left over when one side is longer takes a row of its own beside an empty cell.
std::vector<DiffRow> alignedRows(const DiffHunk& hunk);

} // namespace gk
