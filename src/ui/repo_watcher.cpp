#include "ui/repo_watcher.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <QSocketNotifier>
#include <cerrno>
#include <climits>
#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace gk {
namespace {

constexpr int kSettleMs = 300;
/// A file written without pause (a log, a build) still refreshes this often.
constexpr int kMaxWaitMs = 2000;
constexpr int kMaxFallbackDirectories = 512;

/// Whether a change under a git dir (path relative to it) is one the window shows:
/// refs, HEAD, the index and an operation's markers are; objects, logs, hooks and a
/// lock taken for a write that has not landed yet are not.
bool gitChangeMatters(const QString& relative) {
    if (relative.isEmpty() || relative.endsWith(QStringLiteral(".lock"))) {
        return false;
    }
    for (const char* noise : {"objects", "logs", "hooks", "lfs", "modules", "worktrees",
                              "gitmycelium-rewrite", "COMMIT_EDITMSG", "fsmonitor--daemon"}) {
        const QString prefix = QString::fromLatin1(noise);
        if (relative == prefix || relative.startsWith(prefix + QLatin1Char('/'))) {
            return false;
        }
    }
    return true;
}

QString withSlash(const QString& dir) {
    return dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/');
}

QString canonicalOr(const QString& path) {
    if (path.isEmpty()) {
        return path;
    }
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}

/// What git ignores, as absolute paths: whole directories (prefixes) and files.
struct IgnoredSet {
    QStringList directories; ///< each ending in '/'
    QSet<QString> files;

    static IgnoredSet from(const QString& workDir, const QStringList& relative) {
        IgnoredSet set;
        const QString root = withSlash(workDir);
        for (const QString& path : relative) {
            if (path.endsWith(QLatin1Char('/'))) {
                set.directories << root + path;
            } else {
                set.files.insert(root + path);
            }
        }
        return set;
    }

    bool contains(const QString& path) const {
        if (files.contains(path)) {
            return true;
        }
        const QString asDir = withSlash(path);
        for (const QString& dir : directories) {
            if (asDir.startsWith(dir)) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

#if defined(__linux__)

struct RepoWatcher::Native {
    struct Entry {
        std::string path;
        bool git = false;
    };
    /// Shared with the workers that add watches, so the descriptor outlives a scan
    /// that is still running when the roots change.
    struct State {
        int fd = -1;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> ready{false};
        std::atomic<int> count{0};
        int budget = 0;
        std::mutex mutex;
        std::unordered_map<int, Entry> entries;
        IgnoredSet ignored;

        ~State() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    };

    std::shared_ptr<State> state;
    QSocketNotifier* notifier = nullptr;

    static constexpr std::uint32_t kTreeMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE |
                                               IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                                               IN_DELETE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW |
                                               IN_EXCL_UNLINK;
    static constexpr std::uint32_t kGitMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE |
                                              IN_MOVED_FROM | IN_MOVED_TO | IN_ONLYDIR;

    static int watchBudget() {
        int limit = 8192;
        if (FILE* file = std::fopen("/proc/sys/fs/inotify/max_user_watches", "r")) {
            if (std::fscanf(file, "%d", &limit) != 1) {
                limit = 8192;
            }
            std::fclose(file);
        }
        // Half of what the user may hold: an IDE or a second window wants some too.
        return std::max(512, std::min(limit / 2, 500000));
    }

    static bool add(State& state, const std::string& path, bool git) {
        if (state.count.load() >= state.budget) {
            return false;
        }
        const int wd = inotify_add_watch(state.fd, path.c_str(), git ? kGitMask : kTreeMask);
        if (wd < 0) {
            return errno != ENOSPC;
        }
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.entries.emplace(wd, Entry{path, git}).second) {
            ++state.count;
        }
        return true;
    }

    /// Every directory under `root`, `.git` and what git ignores left out.
    static void addTree(State& state, const std::string& root, bool git) {
        std::vector<std::string> pending{root};
        while (!pending.empty() && !state.cancelled.load()) {
            const std::string dir = std::move(pending.back());
            pending.pop_back();
            if (!git) {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.ignored.contains(QString::fromStdString(dir))) {
                    continue;
                }
            }
            if (!add(state, dir, git)) {
                return;
            }
            DIR* handle = ::opendir(dir.c_str());
            if (!handle) {
                continue;
            }
            while (const dirent* entry = ::readdir(handle)) {
                const char* name = entry->d_name;
                if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                    continue;
                }
                if (!git && std::strcmp(name, ".git") == 0) {
                    continue;
                }
                std::string child = dir + '/' + name;
                bool isDir = entry->d_type == DT_DIR;
                if (entry->d_type == DT_UNKNOWN) {
                    struct stat info {};
                    isDir = ::lstat(child.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
                }
                if (isDir) {
                    pending.push_back(std::move(child));
                }
            }
            ::closedir(handle);
        }
    }
};

bool RepoWatcher::startNative() {
    auto state = std::make_shared<Native::State>();
    state->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (state->fd < 0) {
        return false;
    }
    state->budget = Native::watchBudget();
    m_native = std::make_unique<Native>();
    m_native->state = state;
    m_native->notifier = new QSocketNotifier(state->fd, QSocketNotifier::Read, this);
    connect(m_native->notifier, &QSocketNotifier::activated, this, [this] {
        if (!m_native) {
            return;
        }
        const std::shared_ptr<Native::State> current = m_native->state;
        alignas(inotify_event) char buffer[64 * 1024];
        bool any = false;
        bool git = false;
        std::vector<std::string> newDirectories;
        for (;;) {
            const ssize_t length = ::read(current->fd, buffer, sizeof(buffer));
            if (length <= 0) {
                break;
            }
            for (ssize_t offset = 0; offset < length;) {
                const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
                offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
                if (event->mask & IN_Q_OVERFLOW) {
                    any = true;
                    git = true;
                    continue;
                }
                std::lock_guard<std::mutex> lock(current->mutex);
                const auto found = current->entries.find(event->wd);
                if (event->mask & IN_IGNORED) {
                    if (found != current->entries.end()) {
                        current->entries.erase(found);
                        --current->count;
                    }
                    continue;
                }
                if (found == current->entries.end()) {
                    any = true;
                    continue;
                }
                const Native::Entry& entry = found->second;
                const std::string name = event->len > 0 ? std::string(event->name) : std::string();
                const QString full =
                    QString::fromStdString(name.empty() ? entry.path : entry.path + '/' + name);
                if (entry.git) {
                    if (isGitPath(full)) {
                        any = true;
                        git = true;
                    }
                    if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)) &&
                        isGitPath(full)) {
                        newDirectories.push_back(entry.path + '/' + name);
                    }
                    continue;
                }
                if (name == ".git" || current->ignored.contains(full)) {
                    continue;
                }
                any = true;
                if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) {
                    newDirectories.push_back(entry.path + '/' + name);
                }
            }
        }
        if (!newDirectories.empty()) {
            // A directory that just appeared is watched from now on, subtree and all;
            // a checkout can create thousands, so not on this thread.
            QThreadPool::globalInstance()->start([current, newDirectories, gitDir = m_gitDir,
                                                  commonDir = m_commonDir] {
                for (const std::string& dir : newDirectories) {
                    const QString path = QString::fromStdString(dir);
                    const bool git = path.startsWith(withSlash(gitDir)) ||
                                     (!commonDir.isEmpty() && path.startsWith(withSlash(commonDir)));
                    Native::addTree(*current, dir, git);
                }
            });
        }
        if (any) {
            noteChange(git);
        }
    });

    const QString workDir = m_workDir;
    const QString gitDir = m_gitDir;
    const QString commonDir = m_commonDir;
    const IgnoredLister lister = m_ignoredLister;
    QThreadPool::globalInstance()->start([state, workDir, gitDir, commonDir, lister] {
        Native::add(*state, gitDir.toStdString(), true);
        const QString refsRoot = (commonDir.isEmpty() ? gitDir : commonDir) + QStringLiteral("/refs");
        if (!commonDir.isEmpty() && commonDir != gitDir) {
            Native::add(*state, commonDir.toStdString(), true);
        }
        Native::addTree(*state, refsRoot.toStdString(), true);
        if (!workDir.isEmpty()) {
            if (lister) {
                IgnoredSet ignored = IgnoredSet::from(workDir, lister(workDir));
                std::lock_guard<std::mutex> lock(state->mutex);
                state->ignored = std::move(ignored);
            }
            Native::addTree(*state, workDir.toStdString(), false);
        }
        state->ready = true;
    });
    return true;
}

void RepoWatcher::stopNative() {
    if (!m_native) {
        return;
    }
    m_native->state->cancelled = true;
    delete m_native->notifier;
    m_native.reset();
}

int RepoWatcher::nativeDirectoryCount() const {
    return m_native ? m_native->state->count.load() : 0;
}

bool RepoWatcher::nativeReady() const {
    return m_native && m_native->state->ready.load();
}

#elif defined(__APPLE__)

struct RepoWatcher::Native {
    struct Shared {
        QPointer<RepoWatcher> owner;
        std::mutex mutex;
        IgnoredSet ignored;
        QString workDir;
        QString gitDir;
        QString commonDir;
        std::atomic<bool> ready{false};
    };

    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
    std::shared_ptr<Shared> shared;

    static void callback(ConstFSEventStreamRef, void* info, size_t count, void* paths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
        auto* shared = static_cast<Shared*>(info);
        const auto* const* names = static_cast<const char* const*>(paths);
        bool any = false;
        bool git = false;
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            const QString gitRoot = withSlash(shared->gitDir);
            const QString commonRoot =
                shared->commonDir.isEmpty() ? QString() : withSlash(shared->commonDir);
            const QString workRoot = shared->workDir.isEmpty() ? QString() : withSlash(shared->workDir);
            for (size_t i = 0; i < count; ++i) {
                if (flags[i] & (kFSEventStreamEventFlagMustScanSubDirs |
                                kFSEventStreamEventFlagUserDropped |
                                kFSEventStreamEventFlagKernelDropped)) {
                    any = true;
                    git = true;
                    continue;
                }
                const QString path = QString::fromUtf8(names[i]);
                if (path.startsWith(gitRoot) || withSlash(path) == gitRoot) {
                    if (gitChangeMatters(path.mid(gitRoot.size()))) {
                        any = true;
                        git = true;
                    }
                    continue;
                }
                if (!commonRoot.isEmpty() &&
                    (path.startsWith(commonRoot) || withSlash(path) == commonRoot)) {
                    if (gitChangeMatters(path.mid(commonRoot.size()))) {
                        any = true;
                        git = true;
                    }
                    continue;
                }
                if (workRoot.isEmpty() || !path.startsWith(workRoot)) {
                    continue;
                }
                const QString relative = path.mid(workRoot.size());
                if (relative == QStringLiteral(".git") ||
                    relative.startsWith(QStringLiteral(".git/")) ||
                    relative.contains(QStringLiteral("/.git/")) || shared->ignored.contains(path)) {
                    continue;
                }
                any = true;
            }
        }
        if (!any) {
            return;
        }
        QPointer<RepoWatcher> owner = shared->owner;
        if (!owner) {
            return;
        }
        QMetaObject::invokeMethod(
            owner.data(),
            [owner, git] {
                if (owner) {
                    owner->noteChange(git);
                }
            },
            Qt::QueuedConnection);
    }
};

bool RepoWatcher::startNative() {
    auto shared = std::make_shared<Native::Shared>();
    shared->owner = this;
    shared->workDir = m_workDir;
    shared->gitDir = m_gitDir;
    shared->commonDir = m_commonDir == m_gitDir ? QString() : m_commonDir;

    QStringList roots;
    if (!m_workDir.isEmpty()) {
        roots << m_workDir;
    }
    for (const QString& dir : {m_gitDir, shared->commonDir}) {
        if (!dir.isEmpty() && (m_workDir.isEmpty() || !dir.startsWith(withSlash(m_workDir)))) {
            roots << dir;
        }
    }
    CFMutableArrayRef array = CFArrayCreateMutable(nullptr, roots.size(), &kCFTypeArrayCallBacks);
    for (const QString& root : roots) {
        CFStringRef path = root.toCFString();
        CFArrayAppendValue(array, path);
        CFRelease(path);
    }
    FSEventStreamContext context{0, shared.get(), nullptr, nullptr, nullptr};
    FSEventStreamRef stream = FSEventStreamCreate(
        nullptr, &Native::callback, &context, array, kFSEventStreamEventIdSinceNow, 0.15,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(array);
    if (!stream) {
        return false;
    }
    dispatch_queue_t queue = dispatch_queue_create("gitmycelium.watcher", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(stream, queue);
    if (!FSEventStreamStart(stream)) {
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        dispatch_release(queue);
        return false;
    }
    m_native = std::make_unique<Native>();
    m_native->stream = stream;
    m_native->queue = queue;
    m_native->shared = shared;

    const QString workDir = m_workDir;
    const IgnoredLister lister = m_ignoredLister;
    QThreadPool::globalInstance()->start([shared, workDir, lister] {
        if (!workDir.isEmpty() && lister) {
            IgnoredSet ignored = IgnoredSet::from(workDir, lister(workDir));
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->ignored = std::move(ignored);
        }
        shared->ready = true;
    });
    return true;
}

void RepoWatcher::stopNative() {
    if (!m_native) {
        return;
    }
    FSEventStreamStop(m_native->stream);
    FSEventStreamInvalidate(m_native->stream);
    FSEventStreamRelease(m_native->stream);
    // A callback already queued finishes before the context it reads goes away.
    dispatch_sync_f(m_native->queue, nullptr, [](void*) {});
    dispatch_release(m_native->queue);
    m_native.reset();
}

int RepoWatcher::nativeDirectoryCount() const {
    return m_native ? 1 : 0;
}

bool RepoWatcher::nativeReady() const {
    return m_native && m_native->shared->ready.load();
}

#else

struct RepoWatcher::Native {};

bool RepoWatcher::startNative() {
    return false;
}

void RepoWatcher::stopNative() {}

int RepoWatcher::nativeDirectoryCount() const {
    return 0;
}

bool RepoWatcher::nativeReady() const {
    return false;
}

#endif

RepoWatcher::RepoWatcher(QObject* parent) : QObject(parent) {
    m_settle.setSingleShot(true);
    m_settle.setInterval(kSettleMs);
    connect(&m_settle, &QTimer::timeout, this, &RepoWatcher::onSettled);
    connect(&m_fallback, &QFileSystemWatcher::directoryChanged, this, [this](const QString& dir) {
        rewatchFallback();
        noteChange(isGitPath(dir));
    });
    connect(&m_fallback, &QFileSystemWatcher::fileChanged, this,
            [this](const QString& file) { noteChange(isGitPath(file)); });
}

RepoWatcher::~RepoWatcher() {
    stop();
}

void RepoWatcher::setIgnoredLister(IgnoredLister lister) {
    m_ignoredLister = std::move(lister);
}

void RepoWatcher::setRoots(const QString& workDir, const QString& gitDir,
                           const QString& commonDir) {
    const QString work = canonicalOr(workDir);
    const QString git = canonicalOr(gitDir);
    const QString common = canonicalOr(commonDir.isEmpty() ? gitDir : commonDir);
    // A tab switch hands the same roots back; the scan is not worth repeating.
    if ((m_native || m_usingFallback) && work == m_workDir && git == m_gitDir &&
        common == m_commonDir) {
        return;
    }
    stop();
    m_workDir = work;
    m_gitDir = git;
    m_commonDir = common;
    if (m_workDir.isEmpty() && m_gitDir.isEmpty()) {
        return;
    }
    if (!startNative()) {
        startFallback();
    }
}

void RepoWatcher::stop() {
    m_settle.stop();
    m_pendingGitState = false;
    stopNative();
    m_usingFallback = false;
    const QStringList paths = m_fallback.directories() + m_fallback.files();
    if (!paths.isEmpty()) {
        m_fallback.removePaths(paths);
    }
    m_workDir.clear();
    m_gitDir.clear();
    m_commonDir.clear();
}

int RepoWatcher::watchedDirectoryCount() const {
    return m_usingFallback ? static_cast<int>(m_fallback.directories().size())
                           : nativeDirectoryCount();
}

bool RepoWatcher::ready() const {
    return m_usingFallback || nativeReady();
}

bool RepoWatcher::isGitPath(const QString& path) const {
    for (const QString& root : {m_gitDir, m_commonDir}) {
        if (root.isEmpty()) {
            continue;
        }
        const QString prefix = withSlash(root);
        if (path == root) {
            return true;
        }
        if (path.startsWith(prefix)) {
            return gitChangeMatters(path.mid(prefix.size()));
        }
    }
    return false;
}

void RepoWatcher::noteChange(bool gitState) {
    m_pendingGitState = m_pendingGitState || gitState;
    if (!m_settle.isActive()) {
        m_firstPending.start();
    } else if (m_firstPending.elapsed() >= kMaxWaitMs) {
        onSettled();
        return;
    }
    m_settle.start();
}

void RepoWatcher::onSettled() {
    m_settle.stop();
    const bool gitState = m_pendingGitState;
    m_pendingGitState = false;
    emit changed(gitState);
}

void RepoWatcher::startFallback() {
    m_usingFallback = true;
    rewatchFallback();
}

void RepoWatcher::collectFallback(const QString& root, QStringList* out) const {
    QStringList pending{root};
    while (!pending.isEmpty() && out->size() < kMaxFallbackDirectories) {
        const QString dir = pending.takeFirst();
        out->append(dir);
        const QFileInfoList children =
            QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& child : children) {
            if (child.fileName() == QStringLiteral(".git")) {
                continue;
            }
            if (out->size() + pending.size() >= kMaxFallbackDirectories) {
                break;
            }
            pending.append(child.absoluteFilePath());
        }
    }
}

void RepoWatcher::rewatchFallback() {
    if (!m_usingFallback) {
        return;
    }
    QStringList wanted;
    if (!m_workDir.isEmpty()) {
        collectFallback(m_workDir, &wanted);
    }
    if (!m_gitDir.isEmpty()) {
        wanted << m_gitDir;
        const QString refs = m_commonDir + QStringLiteral("/refs");
        if (QFileInfo(refs).isDir()) {
            collectFallback(refs, &wanted);
        }
    }
    const QStringList watchedDirs = m_fallback.directories();
    QStringList newDirs;
    for (const QString& dir : wanted) {
        if (!watchedDirs.contains(dir)) {
            newDirs << dir;
        }
    }
    if (!newDirs.isEmpty()) {
        m_fallback.addPaths(newDirs);
    }
    const QStringList watchedFiles = m_fallback.files();
    QStringList newFiles;
    for (const QString& name : {QStringLiteral("index"), QStringLiteral("HEAD")}) {
        if (m_gitDir.isEmpty()) {
            break;
        }
        const QString file = m_gitDir + QLatin1Char('/') + name;
        if (QFileInfo::exists(file) && !watchedFiles.contains(file)) {
            newFiles << file;
        }
    }
    if (!newFiles.isEmpty()) {
        m_fallback.addPaths(newFiles);
    }
}

} // namespace gk
