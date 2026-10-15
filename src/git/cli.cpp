#include "git/cli.h"

#include <array>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <thread>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace gk {
namespace {

/// The variables this layer depends on, set in every child over the reader's own.
///
/// `LANGUAGE=en` keeps git's messages in English without touching `LC_CTYPE`, so hooks,
/// editors and tools git starts still see the reader's character set — `LC_ALL=C` would
/// hand a Python hook an ASCII locale. `GIT_OPTIONAL_LOCKS=0` stops a background read
/// from taking `index.lock` under a command the reader runs at the same time.
/// `GIT_TERMINAL_PROMPT=0` makes a credential prompt fail instead of waiting on the
/// terminal the GUI was started from, where nobody is looking.
constexpr std::array<std::string_view, 3> kOverrides = {"LANGUAGE=en", "GIT_OPTIONAL_LOCKS=0",
                                                        "GIT_TERMINAL_PROMPT=0"};

/// A streamed `git log` writes each record as it finds it, not when its buffer fills:
/// a rare match would otherwise wait for the end of the history.
std::vector<std::string_view> overridesFor(bool streaming) {
    std::vector<std::string_view> overrides(kOverrides.begin(), kOverrides.end());
    if (streaming) {
        overrides.push_back("GIT_FLUSH=1");
    }
    return overrides;
}

} // namespace

#ifdef _WIN32

namespace {

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string systemMessage(DWORD code) {
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string message = length > 0 ? std::string(buffer, length) : std::to_string(code);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

/// One argument as the C runtime's parser reads it back (git is a MinGW program):
/// quoted when it holds a space, a tab or a quote, backslashes doubled only before
/// a quote.
void appendQuoted(std::wstring* line, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line->append(arg);
        return;
    }
    line->push_back(L'"');
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            line->append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            line->append(backslashes * 2 + 1, L'\\');
        } else {
            line->append(backslashes, L'\\');
        }
        line->push_back(*it);
    }
    line->push_back(L'"');
}

/// The reader's environment with the overrides, as CreateProcessW's block. Names are
/// case-insensitive on Windows.
std::wstring childEnvironment(bool streaming) {
    const std::vector<std::string_view> overrides = overridesFor(streaming);
    std::wstring block;
    if (wchar_t* strings = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = strings; *entry; entry += wcslen(entry) + 1) {
            const std::wstring_view variable(entry);
            bool overridden = false;
            for (const std::string_view override : overrides) {
                const std::wstring name = widen(override.substr(0, override.find('=') + 1));
                if (variable.size() >= name.size() &&
                    CompareStringOrdinal(variable.data(), static_cast<int>(name.size()),
                                         name.data(), static_cast<int>(name.size()),
                                         TRUE) == CSTR_EQUAL) {
                    overridden = true;
                    break;
                }
            }
            if (!overridden) {
                block.append(variable);
                block.push_back(L'\0');
            }
        }
        FreeEnvironmentStringsW(strings);
    }
    for (const std::string_view override : overrides) {
        block.append(widen(override));
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

void readAll(HANDLE pipe, std::string* sink) {
    std::array<char, 65536> buffer{};
    DWORD n = 0;
    while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr) && n > 0) {
        sink->append(buffer.data(), n);
    }
}

void readInto(HANDLE pipe, HANDLE process, const OutputSink& sink) {
    std::array<char, 65536> buffer{};
    DWORD n = 0;
    bool wanted = true;
    while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr) && n > 0) {
        if (wanted && !sink(std::string_view(buffer.data(), n))) {
            wanted = false;
            TerminateProcess(process, 1);
        }
    }
}

} // namespace

ProcessResult runGit(const std::vector<std::string>& args, const std::string& workDir) {
    return runGit(args, workDir, OutputSink());
}

ProcessResult runGit(const std::vector<std::string>& args, const std::string& workDir,
                     const OutputSink& sink) {
    ProcessResult result;

    std::wstring commandLine = L"git";
    std::vector<std::string> argv;
    if (!workDir.empty()) {
        argv.push_back("-C");
        argv.push_back(workDir);
    }
    argv.insert(argv.end(), args.begin(), args.end());
    for (const std::string& arg : argv) {
        commandLine.push_back(L' ');
        appendQuoted(&commandLine, widen(arg));
    }
    std::wstring environment = childEnvironment(static_cast<bool>(sink));

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             OPEN_EXISTING, 0, nullptr);
    const auto closeAll = [&] {
        for (HANDLE handle : {outRead, outWrite, errRead, errWrite, nul}) {
            if (handle && handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
        }
    };
    if (nul == INVALID_HANDLE_VALUE || !CreatePipe(&outRead, &outWrite, &inheritable, 0) ||
        !CreatePipe(&errRead, &errWrite, &inheritable, 0)) {
        result.err = "failed to spawn git: " + systemMessage(GetLastError());
        closeAll();
        return result;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    // The child inherits these three handles and nothing else. Inheritance is
    // otherwise all-or-nothing per process, and a git spawned by another thread at
    // the same moment would keep our pipe's write end open after our child exits,
    // leaving the read below waiting on a pipe nobody writes to.
    std::array<HANDLE, 3> inherited = {nul, outWrite, errWrite};
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<char> attributeStorage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize);
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                              sizeof(HANDLE) * inherited.size(), nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul;
    startup.StartupInfo.hStdOutput = outWrite;
    startup.StartupInfo.hStdError = errWrite;
    startup.lpAttributeList = attributes;

    // No console: a credential or host-key question has nowhere to wait, so it fails
    // at once (the counterpart of the POSIX child leading its own session).
    PROCESS_INFORMATION process{};
    const BOOL spawned = CreateProcessW(
        nullptr, commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        environment.data(), nullptr, &startup.StartupInfo, &process);
    const DWORD spawnError = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    CloseHandle(nul);
    outWrite = errWrite = nul = nullptr;
    if (!spawned) {
        result.err = "failed to spawn git: " + systemMessage(spawnError);
        closeAll();
        return result;
    }
    CloseHandle(process.hThread);

    // Read together, not one after the other: a child that fills the stderr pipe
    // while we block on stdout never exits.
    std::thread errReader(readAll, errRead, &result.err);
    if (sink) {
        readInto(outRead, process.hProcess, sink);
    } else {
        readAll(outRead, &result.out);
    }
    errReader.join();

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    if (GetExitCodeProcess(process.hProcess, &exitCode)) {
        result.exitCode = static_cast<int>(exitCode);
    } else {
        result.err += "failed to collect git's exit status";
    }
    CloseHandle(process.hProcess);
    closeAll();
    return result;
}

#else

namespace {

std::vector<std::string> childEnvironment(bool streaming) {
    const std::vector<std::string_view> overrides = overridesFor(streaming);
    std::vector<std::string> env;
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string_view variable(*entry);
        bool overridden = false;
        for (const std::string_view override : overrides) {
            const std::string_view name = override.substr(0, override.find('=') + 1);
            if (variable.substr(0, name.size()) == name) {
                overridden = true;
                break;
            }
        }
        if (!overridden) {
            env.emplace_back(variable);
        }
    }
    for (const std::string_view override : overrides) {
        env.emplace_back(override);
    }
    return env;
}

std::vector<char*> pointersTo(std::vector<std::string>& strings) {
    std::vector<char*> pointers;
    pointers.reserve(strings.size() + 1);
    for (std::string& s : strings) {
        pointers.push_back(s.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

/// Drains both pipes until the child closes them. Read together, not one after the
/// other: a child that fills the stderr pipe while we block on stdout never exits.
void drain(int outFd, int errFd, std::string* out, std::string* err, const OutputSink& sink,
           pid_t pid) {
    std::array<pollfd, 2> fds = {pollfd{outFd, POLLIN, 0}, pollfd{errFd, POLLIN, 0}};
    bool wanted = true;
    std::array<std::string*, 2> sinks = {out, err};
    std::array<char, 65536> buffer{};
    int open = 2;
    while (open > 0) {
        if (poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if (fds[i].fd < 0 || fds[i].revents == 0) {
                continue;
            }
            const ssize_t n = read(fds[i].fd, buffer.data(), buffer.size());
            if (n > 0 && i == 0 && sink) {
                if (wanted && !sink(std::string_view(buffer.data(), static_cast<std::size_t>(n)))) {
                    wanted = false;
                    kill(pid, SIGKILL);
                }
            } else if (n > 0) {
                sinks[i]->append(buffer.data(), static_cast<std::size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open;
            }
        }
    }
    for (const pollfd& fd : fds) {
        if (fd.fd >= 0) {
            close(fd.fd);
        }
    }
}

/// A pipe whose ends do not survive `exec`. `pipe2` is Linux and FreeBSD only, so
/// elsewhere the flag goes on with `fcntl`; the child additionally closes the ends
/// through the spawn's file actions, because between `pipe` and `fcntl` a git spawned
/// by another thread can inherit the write end and keep it open after our child exits,
/// which would leave `drain` waiting on a pipe nobody is writing to.
int makePipe(int fds[2]) {
#if defined(__linux__) || defined(__FreeBSD__)
    return pipe2(fds, O_CLOEXEC);
#else
    if (pipe(fds) != 0) {
        return -1;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

/// Closes `fd` in the child as part of the spawn. Only above the standard streams:
/// one of them could be 0/1/2 in a process that started with them closed, and closing
/// the descriptor we just `dup2`'d onto would break the child's stdout.
void closeInChild(posix_spawn_file_actions_t* actions, int fd) {
    if (fd > STDERR_FILENO) {
        posix_spawn_file_actions_addclose(actions, fd);
    }
}

} // namespace

ProcessResult runGit(const std::vector<std::string>& args, const std::string& workDir) {
    return runGit(args, workDir, OutputSink());
}

ProcessResult runGit(const std::vector<std::string>& args, const std::string& workDir,
                     const OutputSink& sink) {
    ProcessResult result;

    std::vector<std::string> argv = {"git"};
    if (!workDir.empty()) {
        argv.push_back("-C");
        argv.push_back(workDir);
    }
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<std::string> env = childEnvironment(static_cast<bool>(sink));
    std::vector<char*> argvPointers = pointersTo(argv);
    std::vector<char*> envPointers = pointersTo(env);

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (makePipe(outPipe) != 0 || makePipe(errPipe) != 0) {
        for (const int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) {
                close(fd);
            }
        }
        result.err = std::string("failed to spawn git: ") + std::strerror(errno);
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
    closeInChild(&actions, outPipe[0]);
    closeInChild(&actions, outPipe[1]);
    closeInChild(&actions, errPipe[0]);
    closeInChild(&actions, errPipe[1]);

    // A session of its own, so git and whatever it starts have no controlling
    // terminal. ssh does not ask through stdin: it opens /dev/tty, and for a GUI
    // started from a shell that was the shell's terminal — an unknown host key's
    // question waited there forever and the fetch never returned. Without a terminal
    // ssh asks through SSH_ASKPASS when one is set up, and otherwise fails at once.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif

    pid_t pid = -1;
    const int spawned = posix_spawnp(&pid, "git", &actions, &attributes, argvPointers.data(),
                                     envPointers.data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    close(errPipe[1]);
    if (spawned != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        result.err = std::string("failed to spawn git: ") + std::strerror(spawned);
        return result;
    }

    drain(outPipe[0], errPipe[0], &result.out, &result.err, sink, pid);

    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        result.exitCode = -1;
        result.err += "failed to collect git's exit status";
    } else if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else {
        result.exitCode = -1;
        result.err += WIFSIGNALED(status) ? "git was killed by a signal" : "git did not exit";
    }
    return result;
}

#endif

std::string failureText(const ProcessResult& result) {
    const std::string& text = result.err.empty() ? result.out : result.err;
    const std::size_t end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}

} // namespace gk
