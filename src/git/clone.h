#pragma once

#include <string>

namespace gk {

struct CloneOptions {
    /// `--filter=blob:none`: blobs are fetched on demand.
    bool partial = false;
    std::string branch;
    /// `--depth <n>`. Zero means the whole history.
    int depth = 0;
    /// `--shallow-since=<date>`. Wins over `depth`: git refuses the two together.
    std::string shallowSince;
};

struct CloneResult {
    bool ok = false;
    int exitCode = -1;
    std::string output;
};

/// Clones `url` into `dest` with the `git` binary: it honours the reader's credential
/// helpers, SSH agent, proxy and filters, which libgit2 would ignore. `dest` is created
/// by git; it must not already exist.
CloneResult cloneRepository(const std::string& url, const std::string& dest,
                            const CloneOptions& options = {});

} // namespace gk
