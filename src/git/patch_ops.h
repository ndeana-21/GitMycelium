#pragma once

#include "git/remote.h"

#include <string>

namespace gk {

/// Applying a patch file, delegated to the `git` binary.
///
/// "Apply a patch" is two different operations wearing one name. A plain diff
/// belongs in the working tree, where the reader reviews it and commits; the output
/// of `git format-patch` is a mailbox that carries the author, date and message, so
/// applying it as a diff would throw all of that away — it goes through `git am`
/// and makes the commits it describes.
class PatchOps {
public:
    explicit PatchOps(std::string workDir);

    static bool isMailbox(const std::string& file);

    SyncResult apply(const std::string& file) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

} // namespace gk
