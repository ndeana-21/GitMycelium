#pragma once

#include "core/types.h"

#include <string_view>
#include <vector>

namespace gk {

/// Reads `git status --porcelain=v2 -z` into the shape the rest of the app was
/// written against: one entry per side that changed (staged, then unstaged), a
/// conflict as one unstaged `Conflicted` entry, an untracked path as `Untracked`,
/// a rename under its new path, ordered by path.
std::vector<FileChange> parsePorcelainStatus(std::string_view text);

} // namespace gk
