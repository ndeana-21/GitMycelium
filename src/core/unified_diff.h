#pragma once

#include "core/types.h"

#include <string>
#include <vector>

namespace gk {

/// Parses unified-diff text — the output of `git diff` / `git show` — into the same
/// `FileDiff` shape libgit2 produces.
///
/// This exists for partial (promisor) clones, where libgit2 cannot read a blob that
/// was filtered out and the read path has to be the git binary. It fills the fields
/// the app actually reads: the paths, the change kind, the modes, `binary`, the
/// added/removed counts, and each hunk's range with its lines' kinds, text, line
/// numbers and no-newline flag. Line numbers match libgit2: the side a line is not
/// on is -1.
///
/// The caller is expected to invoke git with `--no-renames` (and no external or
/// coloured diff) so the CLI result agrees with the libgit2 path it replaces; this
/// parser is a reader, not a policy.
std::vector<FileDiff> parseUnifiedDiff(const std::string& text);

/// Parses `git diff --name-status` output into the same `ChangedFile` shape
/// `Repository::changedFiles` builds from libgit2. One `<status>\t<path>` line per
/// file (`R`/`C` carry two paths, old then new, which the caller suppresses with
/// `--no-renames`). `path` is the new path, and `oldPath` the same for anything but a
/// rename — which is how libgit2 names the sides.
std::vector<ChangedFile> parseNameStatus(const std::string& text);

} // namespace gk
