#include "git/submodule.h"

#include "git/cli.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>

namespace gk {
namespace {

SyncResult toSyncResult(const ProcessResult& process) {
    SyncResult result;
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

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(std::move(line));
    }
    return out;
}

struct RawConfig {
    std::string name;
    std::string path;
    std::string url;
    std::string branch;
};

/// Splits `submodule.<name>.<field> <value>`. The field is the *last* dotted
/// component, because a submodule name may itself contain dots or slashes
/// (`submodule.libs/foo.url`): splitting on the first dot would read the name wrong.
bool parseGitmodulesLine(const std::string& line, std::string* name, std::string* field,
                         std::string* value) {
    const std::size_t space = line.find(' ');
    if (space == std::string::npos) {
        return false;
    }
    const std::string key = line.substr(0, space);
    *value = line.substr(space + 1);
    const std::string prefix = "submodule.";
    if (key.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::string rest = key.substr(prefix.size());
    const std::size_t dot = rest.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    *name = rest.substr(0, dot);
    *field = rest.substr(dot + 1);
    return true;
}

/// Reads `<workDir>/<dir>/.gitmodules` and returns its sections keyed by the path
/// *relative to `<dir>`*. A nested `.gitmodules` records paths inside itself, so the
/// caller prefixes them with the containing submodule's path.
std::map<std::string, RawConfig> readGitmodules(const std::string& workDir,
                                                const std::string& dir) {
    std::map<std::string, RawConfig> out;
    std::string file = workDir;
    if (!file.empty() && file.back() != '/') {
        file += '/';
    }
    if (!dir.empty()) {
        file += dir;
        if (file.back() != '/') {
            file += '/';
        }
    }
    file += ".gitmodules";

    const ProcessResult process =
        runGit({"config", "-f", file, "--get-regexp", R"(^submodule\..*\.(path|url|branch)$)"},
               workDir);
    if (process.exitCode != 0) {
        return out; // no `.gitmodules`, or nothing that matches
    }

    std::map<std::string, RawConfig> byName;
    for (const std::string& line : splitLines(process.out)) {
        if (line.empty()) {
            continue;
        }
        std::string name;
        std::string field;
        std::string value;
        if (!parseGitmodulesLine(line, &name, &field, &value)) {
            continue;
        }
        RawConfig& config = byName[name];
        config.name = name;
        if (field == "path") {
            config.path = value;
        } else if (field == "url") {
            config.url = value;
        } else if (field == "branch") {
            config.branch = value;
        }
    }
    for (auto& [name, config] : byName) {
        if (!config.path.empty()) {
            out[config.path] = config;
        }
    }
    return out;
}

std::string absolute(const std::string& workDir, const std::string& rel) {
    std::string out = workDir;
    if (!out.empty() && out.back() != '/') {
        out += '/';
    }
    out += rel;
    return out;
}

} // namespace

SubmoduleOps::SubmoduleOps(std::string workDir) : m_workDir(std::move(workDir)) {}

std::vector<SubmoduleEntry> SubmoduleOps::list() const {
    std::vector<SubmoduleEntry> out;
    // No `.gitmodules`, no submodules: the binary would read the whole index to say
    // so, on every refresh of a repository that has none.
    if (m_workDir.empty() ||
        !std::filesystem::exists(std::filesystem::path(m_workDir) / ".gitmodules")) {
        return out;
    }
    const ProcessResult process = runGit({"submodule", "status", "--recursive"}, m_workDir);
    if (process.exitCode != 0) {
        return out;
    }

    struct Status {
        SubmoduleState state = SubmoduleState::InSync;
        std::string oid;
        std::string path;
        std::string describe;
    };
    std::vector<Status> statuses;
    for (const std::string& line : splitLines(process.out)) {
        if (line.empty()) {
            continue;
        }
        Status status;
        switch (line[0]) {
        case ' ': status.state = SubmoduleState::InSync; break;
        case '+': status.state = SubmoduleState::OutOfSync; break;
        case '-': status.state = SubmoduleState::NotInitialized; break;
        case 'U': status.state = SubmoduleState::Conflict; break;
        default: continue; // not a status line
        }
        std::string rest = line.substr(1);
        const std::size_t space = rest.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        status.oid = rest.substr(0, space);
        rest = rest.substr(space + 1);
        // `path` may be followed by " (describe)". The describe is the last
        // parenthesised group, and a path does not end in ")".
        const std::size_t paren = rest.rfind(" (");
        if (paren != std::string::npos && rest.back() == ')') {
            status.describe = rest.substr(paren + 1);
            rest = rest.substr(0, paren);
        }
        status.path = rest;
        if (!status.path.empty()) {
            statuses.push_back(std::move(status));
        }
    }

    std::vector<std::string> paths;
    paths.reserve(statuses.size());
    for (const Status& status : statuses) {
        paths.push_back(status.path);
    }

    std::map<std::string, std::map<std::string, RawConfig>> configs; // dir -> path -> config
    const auto configFor = [&](const std::string& dir) -> const std::map<std::string, RawConfig>& {
        auto it = configs.find(dir);
        if (it == configs.end()) {
            it = configs.emplace(dir, readGitmodules(m_workDir, dir)).first;
        }
        return it->second;
    };

    // The index gitlinks, read once per repository level: a nested submodule's
    // gitlink lives in *its* parent's index, not the top-level one.
    std::map<std::string, std::map<std::string, std::string>> gitlinks;
    const auto gitlinksFor =
        [&](const std::string& dir) -> const std::map<std::string, std::string>& {
        auto it = gitlinks.find(dir);
        if (it != gitlinks.end()) {
            return it->second;
        }
        std::map<std::string, std::string> found;
        const ProcessResult files = runGit({"ls-files", "--stage"}, absolute(m_workDir, dir));
        if (files.exitCode == 0) {
            // "<mode> <oid> <stage>\t<path>": the tab separates the path, so a path
            // with spaces stays whole. Only gitlinks (mode 160000) matter here.
            for (const std::string& line : splitLines(files.out)) {
                const std::size_t tab = line.find('\t');
                if (tab == std::string::npos) {
                    continue;
                }
                std::istringstream fields(line.substr(0, tab));
                std::string mode;
                std::string oid;
                std::string stage;
                if (!(fields >> mode >> oid >> stage) || mode != "160000") {
                    continue;
                }
                const std::string filePath = line.substr(tab + 1);
                if (stage == "0" || !found.count(filePath)) {
                    found[filePath] = oid; // stage 0 is the resolved entry
                }
            }
        }
        return gitlinks.emplace(dir, std::move(found)).first->second;
    };

    for (const Status& status : statuses) {
        SubmoduleEntry entry;
        entry.path = status.path;
        entry.describe = status.describe;
        entry.state = status.state;
        entry.initialized = status.state != SubmoduleState::NotInitialized;
        // `git submodule status` prints the checked-out commit for an initialized
        // submodule; the recorded gitlink has to come from the index.
        entry.checkedOutOid = entry.initialized ? status.oid : std::string();

        // The containing repository is the longest other path that is a proper
        // prefix of this one; empty means the top level.
        std::string parentDir;
        for (const std::string& other : paths) {
            if (other.size() >= status.path.size()) {
                continue;
            }
            if (status.path.compare(0, other.size(), other) == 0 &&
                status.path[other.size()] == '/' && other.size() > parentDir.size()) {
                parentDir = other;
            }
        }
        entry.nested = !parentDir.empty();
        entry.parentPath = parentDir;

        std::string rel = status.path;
        if (!parentDir.empty() && rel.rfind(parentDir + "/", 0) == 0) {
            rel = rel.substr(parentDir.size() + 1);
        }
        const auto& map = configFor(parentDir);
        const auto config = map.find(rel);
        if (config != map.end()) {
            entry.name = config->second.name;
            entry.url = config->second.url;
            entry.branch = config->second.branch;
        } else {
            entry.name = rel;
        }

        const auto& links = gitlinksFor(parentDir);
        const auto link = links.find(rel);
        entry.recordedOid = link != links.end() ? link->second : status.oid;

        out.push_back(std::move(entry));
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const SubmoduleEntry& a, const SubmoduleEntry& b) { return a.path < b.path; });
    return out;
}

SyncResult SubmoduleOps::add(const std::string& url, const std::string& path,
                             const std::string& branch, bool force) const {
    std::vector<std::string> args{"submodule", "add"};
    if (force) {
        args.push_back("--force");
    }
    if (!branch.empty()) {
        args.push_back("-b");
        args.push_back(branch);
    }
    args.push_back("--");
    args.push_back(url);
    if (!path.empty()) {
        args.push_back(path);
    }
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult SubmoduleOps::deinit(const std::string& path, bool force) const {
    std::vector<std::string> args{"submodule", "deinit"};
    if (force) {
        args.push_back("--force");
    }
    args.push_back("--");
    args.push_back(path);
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult SubmoduleOps::update(const std::vector<std::string>& paths, bool initialize,
                                bool remote, bool recursive) const {
    std::vector<std::string> args{"submodule", "update"};
    if (initialize) {
        args.push_back("--init");
    }
    if (remote) {
        args.push_back("--remote");
    }
    if (recursive) {
        args.push_back("--recursive");
    }
    if (!paths.empty()) {
        args.push_back("--");
        for (const std::string& path : paths) {
            args.push_back(path);
        }
    }
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult SubmoduleOps::sync(const std::vector<std::string>& paths, bool recursive) const {
    std::vector<std::string> args{"submodule", "sync"};
    if (recursive) {
        args.push_back("--recursive");
    }
    if (!paths.empty()) {
        args.push_back("--");
        for (const std::string& path : paths) {
            args.push_back(path);
        }
    }
    return toSyncResult(runGit(args, m_workDir));
}

} // namespace gk
