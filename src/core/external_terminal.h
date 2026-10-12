#pragma once

#include <functional>
#include <string>
#include <vector>

namespace gk {

/// A command that opens a terminal emulator in a directory. `valid` is false when no
/// emulator was found.
struct TerminalLaunch {
    std::string program;
    std::vector<std::string> args;
    bool valid = false;
};

/// Chooses a terminal emulator and the arguments that open it in `workDir`.
///
/// `terminalEnv` is the value of `$TERMINAL` (empty when unset) and is preferred; the
/// well-known emulators are tried after it. `programPath` resolves a bare program name
/// to an executable path, or empty when it is not installed — the app passes
/// `QStandardPaths::findExecutable`, a test passes a map.
///
/// Each emulator spells "start here" differently, so the known ones get their own flag;
/// anything else (including an unrecognised `$TERMINAL`) gets a portable
/// `-e sh -c 'cd "$1" && exec "${SHELL:-sh}"'`, whose directory travels as an argument,
/// not as text to quote.
TerminalLaunch buildTerminalLaunch(const std::string& workDir, const std::string& terminalEnv,
                                   const std::function<std::string(const std::string&)>& programPath);

} // namespace gk
