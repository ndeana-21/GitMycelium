#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gk {

struct ProcessResult {
    int exitCode = -1;
    std::string out; ///< byte for byte (NULs included)
    std::string err;
};

/// Never throws; a spawn failure yields exitCode = -1 and the reason in `err`.
ProcessResult runGit(const std::vector<std::string>& args, const std::string& workDir);

/// Hands stdout over as it arrives instead of collecting it; returning false stops
/// git (killed, so the result's exit code is not 0). `out` stays empty.
using OutputSink = std::function<bool(std::string_view chunk)>;
ProcessResult runGit(const std::vector<std::string>& args, const std::string& workDir,
                     const OutputSink& sink);

/// The first line of what git said about a failure: stderr when it wrote any,
/// otherwise stdout.
std::string failureText(const ProcessResult& result);

} // namespace gk
