#pragma once

#include <cstddef>
#include <string>

namespace gk {

struct SemanticMergeResult {
    bool applicable = false;   ///< every input parsed as a top-level object
    bool hasConflicts = false;
    std::size_t conflicts = 0;
    /// The merged document. A key only one side moved is taken from that side,
    /// formatting and all; a key both sides moved differently becomes a diff3
    /// conflict block, which `parseConflictDocument` reads like any other.
    std::string merged;
};

/// Merges `ours`, `base` and `theirs` as JSON objects, top-level key by key.
///
/// The point is `package.json`-shaped conflicts: when the two sides touched
/// different keys, the answer is obvious and a person should not have to read
/// line markers. The exact bytes of every surviving member are kept, so a key
/// nobody touched comes out unchanged.
///
/// Deliberately conservative: values are compared as bytes, arrays are opaque,
/// and anything that is not a single top-level object leaves `applicable` false
/// so the caller keeps the line-based editor rather than inventing content.
SemanticMergeResult semanticMergeJson(const std::string& ours, const std::string& base,
                                      const std::string& theirs);

} // namespace gk
