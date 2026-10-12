#include "core/external_terminal.h"

namespace gk {
namespace {

struct TerminalSpec {
    const char* name;
    const char* flag; ///< ends with '=' for the `--flag=value` form; empty = generic
};

const TerminalSpec kSpecs[] = {
    {"konsole", "--workdir"},
    {"gnome-terminal", "--working-directory="},
    {"xfce4-terminal", "--working-directory="},
    {"tilix", "--working-directory="},
    {"alacritty", "--working-directory="},
    {"kitty", "--directory"},
};

// A concrete emulator beats the alternatives symlink, which beats xterm.
const char* kCandidates[] = {
    "konsole", "gnome-terminal", "xfce4-terminal", "tilix",
    "alacritty", "kitty", "x-terminal-emulator", "xterm",
};

std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

const TerminalSpec* specFor(const std::string& name) {
    for (const TerminalSpec& spec : kSpecs) {
        if (name == spec.name) {
            return &spec;
        }
    }
    return nullptr;
}

TerminalLaunch genericLaunch(const std::string& program, const std::string& workDir) {
    TerminalLaunch launch;
    launch.program = program;
    // The directory travels as an argument, not as text inside the command string, so
    // a path with spaces or quotes needs no escaping.
    launch.args = {"-e", "sh", "-c", "cd \"$1\" && exec \"${SHELL:-sh}\"", "sh", workDir};
    launch.valid = true;
    return launch;
}

TerminalLaunch launchFor(const std::string& program, const std::string& name,
                         const std::string& workDir) {
    const TerminalSpec* spec = specFor(name);
    if (!spec || std::string(spec->flag).empty()) {
        return genericLaunch(program, workDir);
    }
    TerminalLaunch launch;
    launch.program = program;
    const std::string flag = spec->flag;
    if (flag.back() == '=') {
        launch.args = {flag + workDir};
    } else {
        launch.args = {flag, workDir};
    }
    launch.valid = true;
    return launch;
}

std::string resolve(const std::function<std::string(const std::string&)>& programPath,
                    const std::string& name) {
    if (name.empty()) {
        return {};
    }
    // An absolute or relative path is taken as given; a bare name is looked up.
    if (name.find('/') != std::string::npos) {
        return name;
    }
    return programPath(name);
}

} // namespace

TerminalLaunch buildTerminalLaunch(const std::string& workDir, const std::string& terminalEnv,
                                   const std::function<std::string(const std::string&)>& programPath) {
    if (workDir.empty()) {
        return TerminalLaunch{};
    }
    std::vector<std::string> candidates;
    if (!terminalEnv.empty()) {
        candidates.push_back(terminalEnv);
    }
    for (const char* name : kCandidates) {
        candidates.push_back(name);
    }

    for (const std::string& candidate : candidates) {
        const std::string program = resolve(programPath, candidate);
        if (program.empty()) {
            continue;
        }
        return launchFor(program, baseName(candidate), workDir);
    }
    return TerminalLaunch{};
}

} // namespace gk
