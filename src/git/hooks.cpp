#include "git/hooks.h"

#include "git/cli.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#endif

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

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

#ifdef _WIN32
// git for Windows runs a hook that exists, whatever its mode: there is no executable
// bit. A disabled hook is parked beside it as `<name>.disabled`.
const char* const kDisabledSuffix = ".disabled";

std::filesystem::path parked(const std::filesystem::path& hook) {
    return std::filesystem::path(hook.native() + std::filesystem::path(kDisabledSuffix).native());
}

/// Where the hook's text lives: the active file, else the parked one. A hook written
/// fresh is parked, as a fresh file on POSIX has no executable bit.
std::filesystem::path stored(const std::filesystem::path& hook) {
    std::error_code ec;
    return std::filesystem::exists(hook, ec) ? hook : parked(hook);
}

bool present(const std::filesystem::path& hook) {
    std::error_code ec;
    return std::filesystem::exists(hook, ec) || std::filesystem::exists(parked(hook), ec);
}

bool isEnabled(const std::filesystem::path& hook) {
    std::error_code ec;
    return std::filesystem::exists(hook, ec);
}

void setEnabled(const std::filesystem::path& hook, bool on) {
    const std::filesystem::path from = on ? parked(hook) : hook;
    const std::filesystem::path to = on ? hook : parked(hook);
    std::error_code ec;
    if (!std::filesystem::exists(from, ec)) {
        if (std::filesystem::exists(to, ec)) {
            return;
        }
        throw std::runtime_error("the hook file is not there");
    }
    std::filesystem::rename(from, to, ec);
    if (ec) {
        throw std::runtime_error("cannot rename the hook: " + ec.message());
    }
}

bool isParkedName(const std::string& name) {
    const std::size_t n = std::char_traits<char>::length(kDisabledSuffix);
    return name.size() > n && name.compare(name.size() - n, n, kDisabledSuffix) == 0;
}

std::string hookNameOf(const std::string& fileName) {
    return isParkedName(fileName)
               ? fileName.substr(0, fileName.size() - std::char_traits<char>::length(kDisabledSuffix))
               : fileName;
}
#else
std::filesystem::path stored(const std::filesystem::path& hook) {
    return hook;
}

bool present(const std::filesystem::path& hook) {
    std::error_code ec;
    return std::filesystem::exists(hook, ec);
}

bool isEnabled(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    return (info.st_mode & S_IXUSR) != 0;
}

/// Adds or clears the executable bit, keeping the rest of the mode. git asks
/// `access(X_OK)`, which an owner bit answers.
void setEnabled(const std::filesystem::path& path, bool on) {
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

std::string hookNameOf(const std::string& fileName) {
    return fileName;
}
#endif

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
        if (present(entry.path)) {
            entry.state = isEnabled(entry.path) ? HookState::Enabled : HookState::Disabled;
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

    std::error_code ec;
    for (const auto& dirEntry : std::filesystem::directory_iterator(hooksDir, ec)) {
        if (ec) {
            break;
        }
        std::error_code fileError;
        if (!dirEntry.is_regular_file(fileError)) {
            continue;
        }
        const std::string name = hookNameOf(dirEntry.path().filename().string());
        const std::filesystem::path path = hooksDir / name;
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
        entry.state = isEnabled(path) ? HookState::Enabled : HookState::Disabled;
        out.push_back(std::move(entry));
    }
    return out;
}

std::string HooksOps::read(const std::string& name) const {
    const std::string base = dir();
    if (base.empty()) {
        return {};
    }
    return readFile(stored(std::filesystem::path(base) / name));
}

void HooksOps::write(const std::string& name, const std::string& content) const {
    const std::string base = dir();
    if (base.empty()) {
        throw std::runtime_error("cannot find the hooks directory");
    }
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    const std::filesystem::path path = stored(std::filesystem::path(base) / name);
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
    if (!present(hook)) {
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
    setEnabled(hook, true);
}

void HooksOps::disable(const std::string& name) const {
    const std::string base = dir();
    if (base.empty()) {
        throw std::runtime_error("cannot find the hooks directory");
    }
    const std::filesystem::path hook = std::filesystem::path(base) / name;
    if (!present(hook)) {
        return;
    }
    setEnabled(hook, false);
}

} // namespace gk
