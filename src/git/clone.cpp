#include "git/clone.h"

#include "git/cli.h"

#include <vector>

namespace gk {

CloneResult cloneRepository(const std::string& url, const std::string& dest,
                            const CloneOptions& options) {
    std::vector<std::string> args{"clone", "--progress"};
    if (options.partial) {
        args.push_back("--filter=blob:none");
    }
    if (!options.branch.empty()) {
        args.push_back("--branch");
        args.push_back(options.branch);
    }
    if (!options.shallowSince.empty()) {
        // git rejects --depth and --shallow-since together; the date is the more
        // specific request, so a depth that was also given is left out.
        args.push_back("--shallow-since=" + options.shallowSince);
    } else if (options.depth > 0) {
        args.push_back("--depth");
        args.push_back(std::to_string(options.depth));
    }
    // `--` so a repository or directory that begins with a dash is not read as
    // another option.
    args.push_back("--");
    args.push_back(url);
    args.push_back(dest);

    const ProcessResult process = runGit(args, {});
    CloneResult result;
    result.ok = process.exitCode == 0;
    result.exitCode = process.exitCode;
    result.output = process.out;
    if (!process.err.empty()) {
        if (!result.output.empty() && result.output.back() != '\n') {
            result.output += '\n';
        }
        result.output += process.err;
    }
    return result;
}

} // namespace gk
