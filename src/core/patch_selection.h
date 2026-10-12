#pragma once

#include "core/types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace gk {

/// A stretch of changes within one file's diff. `lines` holds indices into
/// `FileDiff::hunks[hunk].lines`; an empty list means the whole hunk. Only
/// Added/Removed lines are meaningful — context is always carried.
struct StageSelection {
    std::size_t hunk = 0;
    std::vector<std::size_t> lines;
};

/// Renders a unified diff carrying only the selected changes of `diff`, in git's
/// own patch format — the form `git apply` and `git_diff_from_buffer` read.
///
/// A single hunk whose unselected changes are dropped is split into as many
/// hunks as it takes for each to stay internally consistent: within a hunk the
/// old-side line numbers (context and removals) and the new-side ones (context
/// and additions) are each consecutive, and dropping a change in the middle
/// breaks that unless a new hunk starts.
///
/// `reverse` swaps the two sides — the patch that takes a staged change back out
/// of the index. `noNewline` markers are carried across, so a file that does not
/// end in a newline keeps that property in the patched side.
///
/// Throws std::runtime_error when a hunk or line index does not exist.
std::string renderPatch(const FileDiff& diff, const std::vector<StageSelection>& selections,
                        bool reverse = false);

} // namespace gk
