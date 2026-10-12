#pragma once

#include <string>
#include <vector>

namespace gk {

struct ProcessResult {
    int exitCode = -1;
    std::string out; ///< byte for byte (NULs included)
    std::string err;
};

/// Never throws; a spawn failure yields exitCode = -1 and the reason in `err`.
ProcessResult runGit(const std::vector<std::string>& args, const std::string& workDir);

/// The first line of what git said about a failure: stderr when it wrote any,
/// otherwise stdout.
std::string failureText(const ProcessResult& result);

} // namespace gk
