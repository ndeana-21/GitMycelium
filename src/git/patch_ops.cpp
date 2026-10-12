#include "git/patch_ops.h"

#include "git/cli.h"

#include <fstream>
#include <sstream>

namespace gk {

PatchOps::PatchOps(std::string workDir) : m_workDir(std::move(workDir)) {}

bool PatchOps::isMailbox(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    // The first non-empty line decides: format-patch writes `From <sha> Mon Sep ...`,
    // a diff writes `diff --git`. Empty lines can precede a hand-edited mailbox.
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            return line.rfind("From ", 0) == 0;
        }
    }
    return false;
}

SyncResult PatchOps::apply(const std::string& file) const {
    SyncResult result;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        result.output = "cannot read patch file: " + file;
        return result;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (buffer.str().empty()) {
        result.output = "the patch file is empty";
        return result;
    }

    // The mailbox path commits; the diff path lands in the working tree and the
    // reader commits. `--3way` on the mailbox lets git reconcile a patch whose
    // context moved; a plain diff is left unstaged so the change can be reviewed
    // and, if it is wrong, discarded like any other working-tree edit.
    const std::vector<std::string> args =
        isMailbox(file) ? std::vector<std::string>{"am", "--3way", file}
                        : std::vector<std::string>{"apply", file};
    const ProcessResult process = runGit(args, m_workDir);
    result.ok = process.exitCode == 0;
    result.exitCode = process.exitCode;
    result.output = process.out + process.err;
    return result;
}

} // namespace gk
