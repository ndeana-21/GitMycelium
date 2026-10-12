#include "git/hooks.h"

#include "git/cli.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

namespace gk {
namespace {

const std::vector<std::string>& clientHooks() {
    static const std::vector<std::string> names = {
        "applypatch-msg", "pre-applypatch", "post-applypatch", "pre-commit",
        "pre-merge-commit", "prepare-commit-msg", "commit-msg", "post-commit",
        "pre-rebase", "post-checkout", "post-merge", "pre-push", "pre-auto-gc",
        "post-rewrite", "post-index-change", "sendemail-validate", "fsmonitor-watchman",
    };
    return names;
}

const std::vector<std::string>& serverHooks() {
    static const std::vector<std::string> names = {
        "pre-receive", "update", "proc-receive", "post-receive", "post-update",
        "push-to-checkout",
    };
    return names;
}

std::string trimTrailing(const std::string& text) {
    std::string out = text;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

bool isExecutable(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    return (info.st_mode & S_IXUSR) != 0;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

/// Adds or clears the executable bit, keeping the rest of the mode. git asks
/// `access(X_OK)`, which an owner bit answers.
void setExecutable(const std::filesystem::path& path, bool on) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        throw std::runtime_error("the hook file is not there");
    }
    mode_t mode = info.st_mode;
    if (on) {
        mode |= S_IXUSR;
    } else {
        mode &= ~static_cast<mode_t>(S_IXUSR);
    }
    if (::chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error("cannot change the hook's permissions");
    }
}

} // namespace

const char* hookStateName(HookState state) {
    switch (state) {
    case HookState::Missing:
        return "missing";
    case HookState::Sample:
        return "sample";
    case HookState::Disabled:
        return "disabled";
    case HookState::Enabled:
        return "enabled";
    }
    return "missing";
}

HooksOps::HooksOps(std::string workDir) : m_workDir(std::move(workDir)) {}

std::string HooksOps::dir() const {
    const ProcessResult process = runGit({"rev-parse", "--git-path", "hooks"}, m_workDir);
    if (process.exitCode != 0) {
        return {};
    }
    const std::string raw = trimTrailing(process.out);
    if (raw.empty()) {
        return {};
    }
    std::filesystem::path path(raw);
    // `rev-parse --git-path` answers relative to where it ran; the default is
    // `.git/hooks`, a configured `core.hooksPath` is relative to the work tree.
    if (path.is_relative() && !m_workDir.empty()) {
        path = std::filesystem::path(m_workDir) / path;
    }
    return path.lexically_normal().string();
}

std::vector<HookEntry> HooksOps::list() const {
    std::vector<HookEntry> out;
    const std::string base = dir();
    const std::filesystem::path hooksDir(base);

    const auto known = [&](const std::string& name, bool serverSide) {
        HookEntry entry;
        entry.name = name;
        entry.serverSide = serverSide;
        if (base.empty()) {
            out.push_back(std::move(entry));
            return;
        }
        entry.path = (hooksDir / name).string();
        std::error_code ec;
        if (std::filesystem::exists(entry.path, ec)) {
            entry.state = isExecutable(entry.path) ? HookState::Enabled : HookState::Disabled;
        } else if (std::filesystem::exists((hooksDir / (name + ".sample")).string(), ec)) {
            entry.state = HookState::Sample;
        }
        out.push_back(std::move(entry));
    };

    for (const std::string& name : clientHooks()) {
        known(name, false);
    }
    for (const std::string& name : serverHooks()) {
        known(name, true);
    }

    if (base.empty()) {
        return out;
    }

    // Anything else the reader put in the directory. A hook the list does not know
    // is still a hook, and hiding it would make the panel disagree with the disk.
    std::error_code ec;
    for (const auto& dirEntry : std::filesystem::directory_iterator(hooksDir, ec)) {
        if (ec) {
            break;
        }
        std::error_code fileError;
        if (!dirEntry.is_regular_file(fileError)) {
            continue;
        }
        const std::filesystem::path path = dirEntry.path();
        const std::string name = path.filename().string();
        if (name.empty() || name.front() == '.') {
            continue;
        }
        if (name.size() > 7 && name.compare(name.size() - 7, 7, ".sample") == 0) {
            continue;
        }
        const bool already = std::any_of(out.begin(), out.end(), [&](const HookEntry& entry) {
            return !entry.path.empty() && std::filesystem::path(entry.path) == path;
        });
        if (already) {
            continue;
        }
        HookEntry entry;
        entry.name = name;
        entry.path = path.string();
        entry.state = isExecutable(path) ? HookState::Enabled : HookState::Disabled;
        out.push_back(std::move(entry));
    }
    return out;
}

std::string HooksOps::read(const std::string& name) const {
    const std::string base = dir();
    if (base.empty()) {
        return {};
    }
    return readFile(std::filesystem::path(base) / name);
}

void HooksOps::write(const std::string& name, const std::string& content) const {
    const std::string base = dir();
    if (base.empty()) {
        throw std::runtime_error("cannot find the hooks directory");
    }
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    const std::filesystem::path path = std::filesystem::path(base) / name;
    if (std::filesystem::is_directory(path, ec)) {
        throw std::runtime_error(name + " is a directory, not a hook");
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write the " + name + " hook");
    }
    out << content;
}

void HooksOps::enable(const std::string& name) const {
    const std::string base = dir();
    if (base.empty()) {
        throw std::runtime_error("cannot find the hooks directory");
    }
    const std::filesystem::path hook = std::filesystem::path(base) / name;
    std::error_code ec;
    if (!std::filesystem::exists(hook, ec)) {
        const std::filesystem::path sample = std::filesystem::path(base) / (name + ".sample");
        if (!std::filesystem::exists(sample, ec)) {
            throw std::runtime_error("there is no " + name + " hook or sample to enable");
        }
        std::filesystem::copy_file(sample, hook, std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (ec) {
            throw std::runtime_error("cannot copy the " + name + " sample: " + ec.message());
        }
    }
    setExecutable(hook, true);
}

void HooksOps::disable(const std::string& name) const {
    const std::string base = dir();
    if (base.empty()) {
        throw std::runtime_error("cannot find the hooks directory");
    }
    const std::filesystem::path hook = std::filesystem::path(base) / name;
    std::error_code ec;
    if (!std::filesystem::exists(hook, ec)) {
        return;
    }
    setExecutable(hook, false);
}

} // namespace gk
