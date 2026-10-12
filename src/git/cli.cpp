#include "git/cli.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace gk {
namespace {

/// The child's environment: the reader's own, with the variables this layer depends
/// on set explicitly.
///
/// `LANGUAGE=en` keeps git's messages in English without touching `LC_CTYPE`, so hooks,
/// editors and tools git starts still see the reader's character set — `LC_ALL=C` would
/// hand a Python hook an ASCII locale. `GIT_OPTIONAL_LOCKS=0` stops a background read
/// from taking `index.lock` under a command the reader runs at the same time.
/// `GIT_TERMINAL_PROMPT=0` makes a credential prompt fail instead of waiting on the
/// terminal the GUI was started from, where nobody is looking.
std::vector<std::string> childEnvironment() {
    static constexpr std::array<std::string_view, 3> kOverrides = {
        "LANGUAGE=en", "GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0"};
    std::vector<std::string> env;
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string_view variable(*entry);
        bool overridden = false;
        for (const std::string_view override : kOverrides) {
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
    for (const std::string_view override : kOverrides) {
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
void drain(int outFd, int errFd, std::string* out, std::string* err) {
    std::array<pollfd, 2> fds = {pollfd{outFd, POLLIN, 0}, pollfd{errFd, POLLIN, 0}};
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
            if (n > 0) {
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
    ProcessResult result;

    // No shell: the arguments reach git as they are, so nothing needs quoting and
    // the environment is set on git itself rather than on a command before it.
    std::vector<std::string> argv = {"git"};
    if (!workDir.empty()) {
        argv.push_back("-C");
        argv.push_back(workDir);
    }
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<std::string> env = childEnvironment();
    std::vector<char*> argvPointers = pointersTo(argv);
    std::vector<char*> envPointers = pointersTo(env);

    // Close-on-exec, so a git spawned concurrently by another thread does not inherit
    // our write ends and keep these pipes open after this child has exited.
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
    // The dups are in place; the child must not hold the pipe ends themselves.
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

    drain(outPipe[0], errPipe[0], &result.out, &result.err);

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

std::string failureText(const ProcessResult& result) {
    const std::string& text = result.err.empty() ? result.out : result.err;
    const std::size_t end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}

} // namespace gk
