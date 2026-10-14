#include "repo/repo_session.h"

#include "core/conflict_auto.h"
#include "core/patch_selection.h"
#include "core/semantic_merge.h"
#include "git/cli.h"
#include "git/external_tools.h"
#include "git/rerere.h"
#include "git/gitflow.h"
#include "git/lfs.h"
#include "git/sparse.h"
#include "git/stash.h"

#include <QThreadPool>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <sstream>

#include <unordered_set>

namespace gk {

namespace {

// Defined below with the other guarded-write helpers; declared here because the
// staging methods above them use them too.
template <typename Fn>
bool guarded(QMutex* mutex, std::atomic<quint64>* epoch, std::unique_ptr<Repository>& repo,
             QString* errorOut, Fn&& fn);

template <typename Fn>
bool recorded(QMutex* mutex, std::atomic<quint64>* epoch, std::unique_ptr<Repository>& repo,
              Oplog* oplog, const QString& label, QString* errorOut, Fn&& fn);

bool operationPointersIntact(const RefState& current, const RefState& from, const RefState& to);

struct RetireSnapshotOnExit {
    const RepoSession* session;
    ~RetireSnapshotOnExit() { session->invalidateSnapshot(); }
};

/// git spells a true boolean several ways; the config holds what the reader wrote.
bool configBool(Repository& repo, const std::string& key) {
    std::string value = repo.configValue(key);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "true" || value == "1" || value == "yes" || value == "on";
}

} // namespace

RepoSession::RepoSession(QObject* parent) : QObject(parent) {
    qRegisterMetaType<gk::CommitPage>("gk::CommitPage");
    qRegisterMetaType<gk::CommitSearchResults>("gk::CommitSearchResults");
    qRegisterMetaType<gk::RefMetaSnapshot>("gk::RefMetaSnapshot");
}

RepoSession::~RepoSession() {
    // Workers check m_alive under the mutex; clearing the repo first makes any
    // in-flight call a no-op.
    m_alive.store(false);
    // A request that nobody must await (requestRefMeta) may still be reading: it
    // tests m_alive before emitting, but testing a member of a destroyed object is
    // undefined, and the freed stack often still reads `true`. Wait for it here —
    // not under m_mutex, so the worker's own info() call can still take that lock.
    {
        QMutexLocker lock(&m_workerMutex);
        while (m_workersInFlight > 0) {
            m_workersDone.wait(&m_workerMutex);
        }
    }
    QMutexLocker lock(&m_mutex);
    m_repo.reset();
}

bool RepoSession::open(const QString& path, QString* errorOut) {
    try {
        auto repo = Repository::open(path.toStdString());
        const RepoInfo opened = repo->info();
        QMutexLocker lock(&m_mutex);
        m_repo = std::move(repo);
        ++m_snapshotEpoch;
        {
            QMutexLocker pathsLock(&m_pathsMutex);
            m_paths = {opened.gitDir, opened.commonDir, opened.workDir, opened.bare};
        }
        m_isOpen.store(true);
        // A fresh repository gets a fresh log: the pointers an old log remembers
        // belong to another repository.
        m_oplog.clear();
        return true;
    } catch (const std::exception& e) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(e.what());
        }
        return false;
    }
}

bool RepoSession::createRepository(const QString& path, const QString& initialBranch,
                                   QString* errorOut) {
    try {
        // The repository is created and closed again; the caller opens it through
        // the normal path, which gives it a context and a tab.
        Repository::init(path.toStdString(), initialBranch.toStdString());
        return true;
    } catch (const std::exception& e) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(e.what());
        }
        return false;
    }
}

void RepoSession::close() {
    QMutexLocker lock(&m_mutex);
    m_isOpen.store(false);
    ++m_snapshotEpoch;
    m_repo.reset();
    m_oplog.clear();
    QMutexLocker pathsLock(&m_pathsMutex);
    m_paths = {};
}

RepoPaths RepoSession::paths() const {
    QMutexLocker lock(&m_pathsMutex);
    return m_paths;
}

std::shared_ptr<const SessionSnapshot> RepoSession::snapshot() const {
    QMutexLocker lock(&m_snapshotMutex);
    if (m_snapshot && m_snapshot->epoch == m_snapshotEpoch.load()) {
        return m_snapshot;
    }
    return nullptr;
}

void RepoSession::invalidateSnapshot() const {
    ++m_snapshotEpoch;
}

std::shared_ptr<SessionSnapshot> RepoSession::readSnapshot() const {
    auto out = std::make_shared<SessionSnapshot>();
    {
        QMutexLocker lock(&m_mutex);
        if (!m_repo) {
            return nullptr;
        }
        out->epoch = m_snapshotEpoch.load();
        try {
            Repository& repo = *m_repo;
            out->info = repo.info();
            out->refs = repo.refs();
            out->refsByCommit = repo.refsByCommit(HistoryFilter{HistoryScope::AllRefs, {}});
            out->status = repo.submoduleStatus();
            out->upstream = repo.upstreamInfo(std::string{});
            out->merging = repo.mergeInProgress();
            out->rebasing = repo.rebaseInProgress();
            out->squashing = repo.squashInProgress();
            out->targetBranch = QString::fromStdString(repo.targetBranch());
            out->signCommits = configBool(repo, "commit.gpgsign");
            try {
                out->commitTemplate = QString::fromStdString(repo.commitTemplate());
            } catch (const std::exception&) {
                // An unreadable template fills nothing, as on the live path.
            }
            for (const char* key : {"user.email", "user.name"}) {
                out->config.insert(QString::fromLatin1(key),
                                   QString::fromStdString(repo.configValue(key)));
            }
        } catch (const std::exception&) {
            // A read that fails is left to the live path, which reports it.
            return nullptr;
        }
        out->canUndo = m_oplog.canUndo();
        out->canRedo = m_oplog.canRedo();
        out->undoLabel = m_oplog.undoLabel();
        out->redoLabel = m_oplog.redoLabel();
    }
    // The git binary needs only the directories, not the libgit2 handle: `git status`
    // alone is half a second on 300k files, and a diff the reader clicked would wait
    // behind it.
    const std::string& workDir = out->info.workDir;
    try {
        std::vector<FileChange> worktree = Repository::worktreeStatus(workDir);
        worktree.insert(worktree.end(), std::make_move_iterator(out->status.begin()),
                        std::make_move_iterator(out->status.end()));
        out->status = std::move(worktree);
        if (!workDir.empty()) {
            out->stashes = StashOps(workDir).list();
            out->submodules = SubmoduleOps(workDir).list();
            out->remotes = RemoteOps(workDir).remotes();
            const RerereOps rerere(workDir);
            out->rerereEnabled = rerere.enabled();
            out->rerereAutoupdate = rerere.autoupdate();
        }
        const std::string worktreeDir = workDir.empty() ? out->info.gitDir : workDir;
        if (!worktreeDir.empty()) {
            out->worktrees = WorktreeOps(worktreeDir).list(workDir);
            for (const WorktreeEntry& entry : out->worktrees) {
                WorktreeWip wip;
                if (!entry.current && !entry.bare && linkedWorktreeWip(entry, &wip)) {
                    out->linkedWip.push_back(std::move(wip));
                }
            }
        }
    } catch (const std::exception&) {
        return nullptr;
    }
    return out;
}

bool RepoSession::installSnapshot(std::shared_ptr<SessionSnapshot> snapshot) {
    if (!snapshot) {
        return false;
    }
    // Compared under the session lock, which every write holds while it moves the
    // epoch: a write cannot slip between the comparison and the install.
    QMutexLocker lock(&m_mutex);
    if (snapshot->epoch != m_snapshotEpoch.load()) {
        return false;
    }
    QMutexLocker snapshotLock(&m_snapshotMutex);
    m_snapshot = std::move(snapshot);
    return true;
}

void RepoSession::primeSnapshot() {
    installSnapshot(readSnapshot());
}

void RepoSession::requestSnapshot() {
    {
        QMutexLocker lock(&m_snapshotMutex);
        if (m_snapshotRunning) {
            m_snapshotAgain = true;
            return;
        }
        m_snapshotRunning = true;
    }
    if (!startWorker([this] { runSnapshotWorker(); })) {
        QMutexLocker lock(&m_snapshotMutex);
        m_snapshotRunning = false;
    }
}

void RepoSession::runSnapshotWorker() {
    // A write that lands mid-read retires the snapshot; read again rather than hand
    // the window nothing. A repository being written continuously gets a few tries,
    // then the window falls back to reading live.
    bool installed = false;
    for (int attempt = 0; attempt < 4 && !installed && m_alive.load(); ++attempt) {
        installed = installSnapshot(readSnapshot());
    }
    bool again = false;
    {
        QMutexLocker lock(&m_snapshotMutex);
        again = m_snapshotAgain;
        m_snapshotAgain = false;
        if (!again) {
            m_snapshotRunning = false;
        }
    }
    if (!m_alive.load()) {
        return;
    }
    if (installed) {
        emit snapshotReady();
    }
    if (again) {
        runSnapshotWorker();
    }
}

RepoInfo RepoSession::info() const {
    if (const auto cached = snapshot()) {
        return cached->info;
    }
    return readOr(RepoInfo{}, [&](Repository& repo) { return repo.info(); });
}

std::vector<RefInfo> RepoSession::refs() const {
    if (const auto cached = snapshot()) {
        return cached->refs;
    }
    return readOr(std::vector<RefInfo>{}, [&](Repository& repo) { return repo.refs(); });
}

std::vector<FileChange> RepoSession::status() const {
    if (const auto cached = snapshot()) {
        return cached->status;
    }
    return readOr(std::vector<FileChange>{}, [&](Repository& repo) { return repo.status(); });
}

std::vector<ChangedFile> RepoSession::changedFiles(const QString& commitId) const {
    return readOr(std::vector<ChangedFile>{}, [&](Repository& repo) {
        return repo.changedFiles(commitId.toStdString());
    });
}

FileDiff RepoSession::commitFileDiff(const QString& commitId, const QString& path) const {
    return readOr(FileDiff{}, [&](Repository& repo) {
        return repo.commitFileDiff(commitId.toStdString(), path.toStdString());
    });
}

std::vector<ChangedFile> RepoSession::changedFilesBetween(const QString& base,
                                                          const QString& target) const {
    return readOr(std::vector<ChangedFile>{}, [&](Repository& repo) {
        return repo.changedFilesBetween(base.toStdString(), target.toStdString());
    });
}

FileDiff RepoSession::fileDiffBetween(const QString& base, const QString& target,
                                      const QString& path) const {
    return readOr(FileDiff{}, [&](Repository& repo) {
        return repo.fileDiffBetween(base.toStdString(), target.toStdString(), path.toStdString());
    });
}

std::vector<ChangedFile> RepoSession::workdirChangedFilesAgainst(const QString& revision) const {
    return readOr(std::vector<ChangedFile>{}, [&](Repository& repo) {
        return repo.workdirChangedFilesAgainst(revision.toStdString());
    });
}

FileDiff RepoSession::workdirFileDiffAgainst(const QString& revision, const QString& path) const {
    return readOr(FileDiff{}, [&](Repository& repo) {
        return repo.workdirFileDiffAgainst(revision.toStdString(), path.toStdString());
    });
}

bool RepoSession::restoreFile(const QString& revision, const QString& path, bool staged,
                              QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.restoreFile(revision.toStdString(), path.toStdString(), staged);
    });
}

std::vector<BlameLine> RepoSession::blame(const QString& path, const QString& revision) const {
    return readOr(std::vector<BlameLine>{}, [&](Repository& repo) {
        return repo.blame(path.toStdString(), revision.toStdString());
    });
}

std::vector<FileHistoryEntry> RepoSession::fileHistory(const QString& path) const {
    return readOr(std::vector<FileHistoryEntry>{}, [&](Repository& repo) {
        return repo.fileHistory(path.toStdString());
    });
}

FileDiff RepoSession::workdirFileDiff(const QString& path) const {
    return readOr(FileDiff{}, [&](Repository& repo) {
        return repo.workdirFileDiff(path.toStdString());
    });
}

FileDiff RepoSession::stagedFileDiff(const QString& path) const {
    return readOr(FileDiff{}, [&](Repository& repo) {
        return repo.stagedFileDiff(path.toStdString());
    });
}

std::string RepoSession::indexFileContent(const QString& path) const {
    return readOr(std::string{}, [&](Repository& repo) {
        return repo.indexFileContent(path.toStdString());
    });
}

bool RepoSession::applyHunkSelection(const QString& path, int hunkIndex,
                                     const QVector<int>& lineIndices, HunkApply what,
                                     const HunkAnchor& expect, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const std::string target = path.toStdString();
        // Re-derive the diff instead of trusting the one the reader saw: the file
        // may have changed under the click. Staging and discarding both start from the
        // working-tree diff; unstaging starts from the staged one.
        const FileDiff diff = what == HunkApply::Unstage ? repo.stagedFileDiff(target)
                                                         : repo.workdirFileDiff(target);
        if (hunkIndex < 0 || static_cast<std::size_t>(hunkIndex) >= diff.hunks.size()) {
            throw GitError("the file's changes moved; reload the diff");
        }
        const DiffHunk& hunk = diff.hunks[static_cast<std::size_t>(hunkIndex)];
        if (hunk.oldStart != expect.oldStart || hunk.oldLines != expect.oldLines ||
            hunk.newStart != expect.newStart || hunk.newLines != expect.newLines) {
            throw GitError("the file's changes moved; reload the diff");
        }
        StageSelection selection;
        selection.hunk = static_cast<std::size_t>(hunkIndex);
        for (const int index : lineIndices) {
            if (index < 0 || static_cast<std::size_t>(index) >= hunk.lines.size()) {
                throw GitError("the line is no longer in this hunk; reload the diff");
            }
            selection.lines.push_back(static_cast<std::size_t>(index));
        }
        // Staging applies the change as written; unstaging and discarding apply its
        // reverse — out of the index, or out of the working tree.
        const bool reverse = what != HunkApply::Stage;
        const std::string patch = renderPatch(diff, {selection}, reverse);
        if (what == HunkApply::Discard) {
            repo.applyPatchToWorkdir(patch);
        } else {
            repo.applyPatchToIndex(patch);
        }
    });
}

bool RepoSession::stageFile(const QString& path, QString* errorOut) {
    QMutexLocker lock(&m_mutex);
    ++m_snapshotEpoch;
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    try {
        m_repo->stageFile(path.toStdString());
        return true;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

bool RepoSession::unstageFile(const QString& path, QString* errorOut) {
    QMutexLocker lock(&m_mutex);
    ++m_snapshotEpoch;
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    try {
        m_repo->unstageFile(path.toStdString());
        return true;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

bool RepoSession::stageAll(QString* errorOut) {
    QMutexLocker lock(&m_mutex);
    ++m_snapshotEpoch;
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    try {
        m_repo->stageAll();
        return true;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

bool RepoSession::discardFileChanges(const QString& path, QString* errorOut) {
    QMutexLocker lock(&m_mutex);
    ++m_snapshotEpoch;
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    try {
        m_repo->discardFileChanges(path.toStdString());
        return true;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

bool RepoSession::discardAllChanges(QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [](Repository& repo) { repo.discardAllChanges(); });
}

bool RepoSession::createCommit(const QString& message, QString* newCommitOut, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Commit"), errorOut,
                    [&](Repository& repo) {
                        const Oid id =
                            repo.createCommit(message.toStdString(), configBool(repo, "commit.gpgsign"));
                        if (newCommitOut) {
                            *newCommitOut = QString::fromStdString(id);
                        }
                    });
}

bool RepoSession::createCommit(const QString& message, bool sign, QString* newCommitOut,
                               QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Commit"), errorOut,
                    [&](Repository& repo) {
                        const Oid id = repo.createCommit(message.toStdString(), sign);
                        if (newCommitOut) {
                            *newCommitOut = QString::fromStdString(id);
                        }
                    });
}

bool RepoSession::undo(QString* labelOut, QString* errorOut) {
    QMutexLocker lock(&m_mutex);
    ++m_snapshotEpoch;
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    const OplogEntry* entry = m_oplog.undoTop();
    if (!entry) {
        if (errorOut) {
            *errorOut = QStringLiteral("there is nothing to undo");
        }
        return false;
    }
    if (m_repo->info().operation != RepoOperation::None) {
        if (errorOut) {
            *errorOut = QStringLiteral("finish or abort the operation in progress first");
        }
        return false;
    }

    RefState current;
    try {
        current = m_repo->snapshotRefs();
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
    // Somebody moved the pointer this undo would restore. Putting it back would
    // overwrite their work silently, so nothing is done.
    if (!operationPointersIntact(current, entry->before, entry->after)) {
        if (errorOut) {
            *errorOut = QStringLiteral("the repository moved since that operation; nothing undone");
        }
        return false;
    }

    try {
        m_repo->applyRefDelta(entry->before, entry->after, true);
    } catch (const std::exception& error) {
        try {
            m_repo->applyRefDelta(current, entry->before, false);
        } catch (const std::exception&) {
            // Nothing more can be done; the first error is the one worth telling.
        }
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }

    // Read the label before commitUndo() pops the entry out from under us.
    const QString label = entry->label;
    m_oplog.commitUndo();
    if (labelOut) {
        *labelOut = label;
    }
    return true;
}

bool RepoSession::redo(QString* labelOut, QString* errorOut) {
    QMutexLocker lock(&m_mutex);
    ++m_snapshotEpoch;
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    const OplogEntry* entry = m_oplog.redoTop();
    if (!entry) {
        if (errorOut) {
            *errorOut = QStringLiteral("there is nothing to redo");
        }
        return false;
    }
    if (m_repo->info().operation != RepoOperation::None) {
        if (errorOut) {
            *errorOut = QStringLiteral("finish or abort the operation in progress first");
        }
        return false;
    }

    RefState current;
    try {
        current = m_repo->snapshotRefs();
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
    if (!operationPointersIntact(current, entry->after, entry->before)) {
        if (errorOut) {
            *errorOut = QStringLiteral("the repository moved since the undo; nothing redone");
        }
        return false;
    }

    try {
        m_repo->applyRefDelta(entry->after, entry->before, true);
    } catch (const std::exception& error) {
        try {
            m_repo->applyRefDelta(current, entry->after, false);
        } catch (const std::exception&) {
        }
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }

    const QString label = entry->label;
    m_oplog.commitRedo();
    if (labelOut) {
        *labelOut = label;
    }
    return true;
}

bool RepoSession::canUndo() const {
    if (const auto cached = snapshot()) {
        return cached->canUndo;
    }
    QMutexLocker lock(&m_mutex);
    return m_oplog.canUndo();
}

bool RepoSession::canRedo() const {
    if (const auto cached = snapshot()) {
        return cached->canRedo;
    }
    QMutexLocker lock(&m_mutex);
    return m_oplog.canRedo();
}

QString RepoSession::nextUndoLabel() const {
    if (const auto cached = snapshot()) {
        return cached->undoLabel;
    }
    QMutexLocker lock(&m_mutex);
    return m_oplog.undoLabel();
}

QString RepoSession::nextRedoLabel() const {
    if (const auto cached = snapshot()) {
        return cached->redoLabel;
    }
    QMutexLocker lock(&m_mutex);
    return m_oplog.redoLabel();
}

QStringList RepoSession::oplogDescription() const {
    QMutexLocker lock(&m_mutex);
    return m_oplog.describe();
}

bool RepoSession::fileAtRevision(const QString& commitId, const QString& path, std::string* bytes,
                                 QString* errorOut) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    try {
        const std::string content = m_repo->fileAt(commitId.toStdString(), path.toStdString());
        if (bytes) {
            *bytes = content;
        }
        return true;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

bool RepoSession::workingFileContent(const QString& path, std::string* bytes,
                                     QString* errorOut) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    try {
        const std::string content = m_repo->workdirFileContent(path.toStdString());
        if (bytes) {
            *bytes = content;
        }
        return true;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

std::vector<ConflictEntry> RepoSession::conflicts() const {
    return readOr(std::vector<ConflictEntry>{}, [&](Repository& repo) { return repo.conflicts(); });
}

QString RepoSession::conflictFileContent(const QString& oid) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    return QString::fromStdString(m_repo->blobText(oid.toStdString()));
}

long long RepoSession::blobSize(const QString& oid) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return -1;
    }
    return m_repo->blobSize(oid.toStdString());
}

QString RepoSession::conflictMarkedContent(const QString& path, bool* textAvailable) const {
    if (textAvailable) {
        *textAvailable = false;
    }
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    return QString::fromStdString(
        m_repo->conflictMarkedContent(path.toStdString(), textAvailable));
}

namespace {

/// Runs a write operation under the session mutex, converting the git-layer
/// exception into an error string. Keeps `ui` free of GitError handling.
template <typename Fn>
bool guarded(QMutex* mutex, std::atomic<quint64>* epoch, std::unique_ptr<Repository>& repo,
             QString* errorOut, Fn&& fn) {
    QMutexLocker lock(mutex);
    ++*epoch;
    if (!repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    try {
        fn(*repo);
        repo->invalidateCommitWalk();
        return true;
    } catch (const GitError& error) {
        if (errorOut) {
            *errorOut = QString::fromStdString(error.what());
        }
        return false;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

/// For operations that go through the git binary or the filesystem rather than
/// libgit2: the repository is checked under the mutex, then the lock is released
/// while the work runs. A hook file write is not a libgit2 call, so holding the
/// session mutex would only block a read that has nothing to do with it.
template <typename Fn>
bool withDirectory(QMutex* mutex, std::atomic<quint64>* epoch, std::unique_ptr<Repository>& repo,
                   QString* errorOut, Fn&& fn) {
    std::string dir;
    // The work runs outside the lock, so the epoch moves on both sides of it: a
    // snapshot read in between is retired either way.
    struct EpochOnExit {
        std::atomic<quint64>* epoch;
        ~EpochOnExit() { ++*epoch; }
    } onExit{epoch};
    {
        QMutexLocker lock(mutex);
        ++*epoch;
        if (!repo) {
            if (errorOut) {
                *errorOut = QStringLiteral("no repository is open");
            }
            return false;
        }
        const RepoInfo info = repo->info();
        dir = info.workDir.empty() ? info.gitDir : info.workDir;
    }
    if (dir.empty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("the repository has no working directory");
        }
        return false;
    }
    try {
        fn(dir);
        return true;
    } catch (const GitError& error) {
        if (errorOut) {
            *errorOut = QString::fromStdString(error.what());
        }
        return false;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

/// A snapshot for the operation log, valid only when the repository is not in
/// the middle of an operation: the log records completed work, not a conflicted
/// rebase that is still asking to be continued.
struct LogSnapshot {
    bool clean = false;
    RefState state;
};

LogSnapshot captureForLog(Repository& repo) {
    LogSnapshot snapshot;
    try {
        if (repo.info().operation != RepoOperation::None) {
            return snapshot;
        }
        snapshot.state = repo.snapshotRefs();
        snapshot.clean = true;
    } catch (const std::exception&) {
        snapshot.clean = false;
    }
    return snapshot;
}

/// True when every pointer the operation moved still holds the value the
/// operation left. Compared ref by ref, not wholesale, so a fetch that added a
/// remote-tracking branch does not make an old commit un-undoable. The index is
/// deliberately not compared: staging changes it without moving pointers, and
/// it must not block an undo.
bool operationPointersIntact(const RefState& current, const RefState& from, const RefState& to) {
    const bool headMoved = from.detached != to.detached || from.headRef != to.headRef ||
                           from.headOid != to.headOid;
    if (headMoved) {
        if (current.detached != to.detached || current.headRef != to.headRef ||
            current.headOid != to.headOid) {
            return false;
        }
    }
    for (const auto& entry : to.refs) {
        const auto before = from.refs.find(entry.first);
        const bool touched = before == from.refs.end() || before->second != entry.second;
        if (!touched) {
            continue;
        }
        const auto now = current.refs.find(entry.first);
        if (now == current.refs.end() || now->second != entry.second) {
            return false;
        }
    }
    for (const auto& entry : from.refs) {
        if (to.refs.find(entry.first) != to.refs.end()) {
            continue;
        }
        if (current.refs.find(entry.first) != current.refs.end()) {
            return false;
        }
    }
    return true;
}

/// Like guarded(), but records the pointer state around the operation so it can
/// be undone. The label is what the undo shows; an operation that moved nothing
/// records nothing, and one left open on a conflict is remembered only when it
/// finally finishes.
template <typename Fn>
bool recorded(QMutex* mutex, std::atomic<quint64>* epoch, std::unique_ptr<Repository>& repo,
              Oplog* oplog, const QString& label, QString* errorOut, Fn&& fn) {
    QMutexLocker lock(mutex);
    ++*epoch;
    if (!repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    try {
        const LogSnapshot before = captureForLog(*repo);
        fn(*repo);
        repo->invalidateCommitWalk();
        if (oplog && !label.isEmpty()) {
            const LogSnapshot after = captureForLog(*repo);
            oplog->noteStart(before.state, before.clean);
            oplog->noteEnd(label, after.state, after.clean);
        }
        return true;
    } catch (const GitError& error) {
        if (errorOut) {
            *errorOut = QString::fromStdString(error.what());
        }
        return false;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

} // namespace

QString RepoSession::resolveRevision(const QString& revision) const {
    QMutexLocker locker(&m_mutex);
    if (!m_repo) {
        return QString();
    }
    return QString::fromStdString(m_repo->resolveRevision(revision.toStdString()));
}

std::vector<ReflogEntry> RepoSession::reflog(std::size_t limit) const {
    QMutexLocker locker(&m_mutex);
    if (!m_repo) {
        return {};
    }
    try {
        return m_repo->reflog(limit);
    } catch (const std::exception&) {
        // A repository with no reflog entries answers nothing; a failure to read one
        // is not worth taking the window down for.
        return {};
    }
}

bool RepoSession::resolveConflict(const QString& path, const QString& content, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.resolveConflict(path.toStdString(), content.toStdString());
    });
}

bool RepoSession::keepConflictSide(const QString& path, ConflictSide side, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.keepConflictSide(path.toStdString(), side);
    });
}

bool RepoSession::deleteConflictFile(const QString& path, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.removeFile(path.toStdString());
    });
}

QStringList RepoSession::resolvedConflictPaths() const {
    QStringList out;
    for (const std::string& path :
         readOr(std::vector<std::string>{},
                [](Repository& repo) { return repo.resolvedConflictPaths(); })) {
        out << QString::fromStdString(path);
    }
    return out;
}

bool RepoSession::markConflicted(const QString& path, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut,
                   [&](Repository& repo) { repo.markConflicted(path.toStdString()); });
}

bool RepoSession::writeConflictDraft(const QString& path, const QString& content, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.writeWorkdirFile(path.toStdString(), content.toStdString());
    });
}

RepoSession::SemanticJsonMerge RepoSession::semanticMergeJson(const QString& path) const {
    SemanticJsonMerge out;
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return out;
    }
    const std::string side = path.toStdString();
    bool present = false;
    const std::string ours =
        m_repo->conflictStageBytes(side, ConflictSide::Ours, &present);
    const std::string base =
        m_repo->conflictStageBytes(side, ConflictSide::Base, &present);
    const std::string theirs =
        m_repo->conflictStageBytes(side, ConflictSide::Theirs, &present);

    const SemanticMergeResult result = gk::semanticMergeJson(ours, base, theirs);
    out.applicable = result.applicable;
    out.hasConflicts = result.hasConflicts;
    out.conflicts = result.conflicts;
    out.merged = QString::fromStdString(result.merged);
    return out;
}

bool RepoSession::rerereEnabled() const {
    if (const auto cached = snapshot()) {
        return cached->rerereEnabled;
    }
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return false;
    }
    try {
        return RerereOps(m_repo->info().workDir).enabled();
    } catch (const std::exception&) {
        return false;
    }
}

bool RepoSession::rerereAutoupdate() const {
    if (const auto cached = snapshot()) {
        return cached->rerereAutoupdate;
    }
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return false;
    }
    try {
        return RerereOps(m_repo->info().workDir).autoupdate();
    } catch (const std::exception&) {
        return false;
    }
}

QStringList RepoSession::rerereTrackedPaths() const {
    QStringList out;
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return out;
    }
    try {
        for (const std::string& path : RerereOps(m_repo->info().workDir).statusPaths()) {
            out << QString::fromStdString(path);
        }
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

QStringList RepoSession::rerereRemainingPaths() const {
    QStringList out;
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return out;
    }
    try {
        for (const std::string& path : RerereOps(m_repo->info().workDir).remainingPaths()) {
            out << QString::fromStdString(path);
        }
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

bool RepoSession::setRerereEnabled(bool on, QString* errorOut) {
    // rerere is configuration and memory, not a ref: the operation log has nothing
    // to put back, so this goes through guarded(), not recorded().
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        RerereOps(repo.info().workDir).setEnabled(on);
    });
}

bool RepoSession::setRerereAutoupdate(bool on, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        RerereOps(repo.info().workDir).setAutoupdate(on);
    });
}

bool RepoSession::applyRerere(QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const SyncResult result = RerereOps(repo.info().workDir).apply();
        if (!result.ok) {
            throw GitError(result.output.empty() ? "git rerere failed" : result.output);
        }
    });
}

bool RepoSession::forgetRerere(const QString& path, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const SyncResult result = RerereOps(repo.info().workDir).forget(path.toStdString());
        if (!result.ok) {
            throw GitError(result.output.empty() ? "git rerere forget failed" : result.output);
        }
    });
}

bool RepoSession::createBranch(const QString& name, const QString& targetCommit, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Create branch %1").arg(name), errorOut,
                    [&](Repository& repo) {
                        repo.createBranch(name.toStdString(), targetCommit.toStdString());
                    });
}

bool RepoSession::createTrackingBranch(const QString& name, const QString& remote,
                                       QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Create branch %1").arg(name), errorOut,
                    [&](Repository& repo) {
                        repo.createTrackingBranch(name.toStdString(), remote.toStdString());
                    });
}

bool RepoSession::setUpstream(const QString& branch, const QString& upstream, QString* errorOut) {
    // Branch tracking is configuration, not a pointer: the operation log records
    // refs and an index tree, so recording this would undo nothing.
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.setUpstream(branch.toStdString(), upstream.toStdString());
    });
}

RepoSession::AutoResolveOutcome RepoSession::autoResolveTrivialConflicts() {
    AutoResolveOutcome outcome;
    QMutexLocker locker(&m_mutex);
    if (!m_repo) {
        return outcome;
    }

    for (const ConflictEntry& entry : m_repo->conflicts()) {
        const QString path = QString::fromStdString(entry.path);
        bool textAvailable = false;
        const std::string marked = m_repo->conflictMarkedContent(entry.path, &textAvailable);
        if (!textAvailable) {
            outcome.needsAPerson << path;
            continue;
        }

        AutoOptions options;
        options.rebaseStep = m_repo->rebaseInProgress();
        const AutoResolved resolved = autoResolveMarkedConflicts(marked, options);
        if (resolved.leftCount > 0 || hasConflictMarkers(resolved.content)) {
            // Not written back. Half a resolution on disk under a conflict editor
            // that shows the index stages is two answers to the same question.
            outcome.needsAPerson << path;
            outcome.hunksLeft += resolved.leftCount;
            continue;
        }

        try {
            m_repo->resolveConflict(entry.path, resolved.content);
        } catch (const std::exception& error) {
            outcome.needsAPerson << path;
            outcome.hunksLeft += 1;
            Q_UNUSED(error);
            continue;
        }
        outcome.stagedFiles << path;
        outcome.hunksResolved += resolved.resolvedCount;

        // Read the reasons back, not to decide anything but to say why a conflict
        // needed nobody: "same on both sides" and "only the incoming side moved"
        // are different facts and the reader can check them.
        std::vector<std::pair<std::string, int>> counts;
        for (const AutoHunk& hunk : resolved.hunks) {
            if (!hunk.resolved) {
                continue;
            }
            const std::string reason = toString(hunk.reason);
            const auto at = std::find_if(counts.begin(), counts.end(),
                                         [&](const auto& pair) { return pair.first == reason; });
            if (at == counts.end()) {
                counts.emplace_back(reason, 1);
            } else {
                at->second += 1;
            }
        }
        QStringList parts;
        for (const auto& [reason, count] : counts) {
            parts << QStringLiteral("%1 x %2").arg(QString::fromStdString(reason)).arg(count);
        }
        if (!parts.isEmpty()) {
            outcome.resolvedDetail
                << QStringLiteral("%1: %2").arg(path, parts.join(QStringLiteral(", ")));
        }
    }

    outcome.allClear = m_repo->conflicts().empty();
    return outcome;
}

RepoSession::SweepOutcome RepoSession::sweepOperationConflicts(std::size_t maxRounds) {
    SweepOutcome outcome;

    for (std::size_t round = 0; round < maxRounds; ++round) {
        const AutoResolveOutcome pass = autoResolveTrivialConflicts();
        outcome.hunksResolved += pass.hunksResolved;
        for (const QString& line : pass.resolvedDetail) {
            if (!outcome.autoDetail.contains(line)) {
                outcome.autoDetail << line;
            }
        }
        for (const QString& file : pass.stagedFiles) {
            if (!outcome.autoStaged.contains(file)) {
                outcome.autoStaged << file;
            }
        }
        for (const QString& file : pass.needsAPerson) {
            if (!outcome.needsAPersonPaths.contains(file)) {
                outcome.needsAPersonPaths << file;
            }
        }

        if (!pass.allClear) {
            outcome.needsAPerson = true;
            return outcome;
        }

        // Nothing was settled this round. Whatever is going on, continuing is not
        // this function's call: an operation whose conflicts a person already
        // resolved is theirs to finish, and quietly committing it for them is not
        // the favour it looks like.
        if (pass.stagedFiles.isEmpty()) {
            return outcome;
        }

        if (m_repo->rebaseInProgress()) {
            RebaseResult result;
            QString error;
            if (!rebaseContinue(&result, &error)) {
                outcome.error = error;
                outcome.needsAPerson = true;
                return outcome;
            }
            if (result.status == RebaseStatus::Conflicted) {
                continue; // the next commit conflicts too: sweep it as well
            }
            outcome.finished = true;
            return outcome;
        }

        if (mergeInProgress()) {
            QString error;
            if (!commitMerge(QStringLiteral("Merge completed in GitMycelium"), &error)) {
                outcome.error = error;
                outcome.needsAPerson = true;
                return outcome;
            }
            outcome.finished = true;
            return outcome;
        }

        return outcome; // nothing in progress to carry forward
    }

    outcome.needsAPerson = true;
    outcome.error = QStringLiteral("gave up after %1 rounds without reaching the end").arg(maxRounds);
    return outcome;
}

bool RepoSession::checkoutBranch(const QString& name, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Checkout %1").arg(name), errorOut,
                    [&](Repository& repo) { repo.checkoutBranch(name.toStdString()); });
}

bool RepoSession::deleteBranch(const QString& name, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Delete branch %1").arg(name), errorOut,
                    [&](Repository& repo) { repo.deleteBranch(name.toStdString(), false); });
}

bool RepoSession::deleteBranchForced(const QString& name, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Delete branch %1").arg(name), errorOut,
                    [&](Repository& repo) { repo.deleteBranch(name.toStdString(), true); });
}

QStringList RepoSession::branchesMergedInto(const QString& base) const {
    QMutexLocker lock(&m_mutex);
    QStringList out;
    if (!m_repo) {
        return out;
    }

    const RepoInfo repositoryInfo = m_repo->info();
    const QString head = repositoryInfo.detached ? QString()
                                                 : QString::fromStdString(repositoryInfo.headBranch);

    // The base must not propose itself, nor the local branch a remote-tracking base
    // stands for: "delete merged branches" that offered `main` when the target is
    // `origin/main` would be a trap.
    QStringList excluded;
    if (!head.isEmpty()) {
        excluded << head;
    }
    const QString baseText = base.trimmed();
    const QString headsPrefix = QStringLiteral("refs/heads/");
    const QString remotesPrefix = QStringLiteral("refs/remotes/");
    if (baseText.startsWith(headsPrefix)) {
        excluded << baseText.mid(headsPrefix.size());
    } else if (baseText.startsWith(remotesPrefix)) {
        const QString rest = baseText.mid(remotesPrefix.size());
        const int slash = rest.indexOf(QLatin1Char('/'));
        if (slash >= 0) {
            excluded << rest.mid(slash + 1);
        }
    } else if (!baseText.isEmpty()) {
        excluded << baseText;
        const int slash = baseText.indexOf(QLatin1Char('/'));
        if (slash >= 0) {
            excluded << baseText.mid(slash + 1);
        }
    }

    for (const RefInfo& ref : m_repo->refs()) {
        if (ref.kind != RefKind::LocalBranch) {
            continue;
        }
        const QString name = QString::fromStdString(ref.shortName);
        if (excluded.contains(name)) {
            continue;
        }
        try {
            if (m_repo->branchMergedInto(ref.shortName, base.toStdString())) {
                out << name;
            }
        } catch (const std::exception&) {
            // Not a branch this repository can resolve; the batch will say so.
        }
    }
    return out;
}

QStringList RepoSession::unmergedAmong(const QStringList& names, const QString& base) const {
    QMutexLocker lock(&m_mutex);
    QStringList out;
    if (!m_repo) {
        return out;
    }
    for (const QString& name : names) {
        try {
            if (!m_repo->branchMergedInto(name.toStdString(), base.toStdString())) {
                out << name;
            }
        } catch (const std::exception&) {
            // Unknown to this repository: not something to scare the reader with.
        }
    }
    return out;
}

bool RepoSession::deleteBranches(const QStringList& names, const QString& base,
                                 bool forceUnmerged, BranchBatchResult* resultOut,
                                 QString* errorOut) {
    BranchBatchResult local;
    const QString label = names.size() == 1
                              ? QStringLiteral("Delete branch %1").arg(names.front())
                              : QStringLiteral("Delete %1 branches").arg(names.size());
    const std::string baseRev = base.toStdString();
    const bool ok = recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, label, errorOut, [&](Repository& repo) {
        const RepoInfo repositoryInfo = repo.info();
        const std::string head =
            repositoryInfo.detached ? std::string{} : repositoryInfo.headBranch;
        for (const QString& name : names) {
            const std::string branch = name.toStdString();
            if (!head.empty() && branch == head) {
                local.failed << QStringLiteral("%1: the branch you are on").arg(name);
                continue;
            }
            // The classification, not the caller's word, decides force. A branch
            // that failed for another reason must never be force-deleted on the way
            // past; only the ones actually carrying unreachable commits are at risk.
            bool losesCommits = false;
            try {
                losesCommits = !repo.branchMergedInto(branch, baseRev);
            } catch (const std::exception& error) {
                local.failed << QStringLiteral("%1: %2")
                                    .arg(name, QString::fromUtf8(error.what()));
                continue;
            }
            try {
                repo.deleteBranch(branch, forceUnmerged && losesCommits, baseRev);
                local.done << name;
            } catch (const std::exception& error) {
                local.failed << QStringLiteral("%1: %2")
                                    .arg(name, QString::fromUtf8(error.what()));
            }
        }
    });
    if (resultOut) {
        *resultOut = local;
    }
    return ok;
}

bool RepoSession::setUpstreams(const QStringList& names, const QString& remote,
                               BranchBatchResult* resultOut, QString* errorOut) {
    BranchBatchResult local;
    const bool ok = guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        for (const QString& name : names) {
            const QString upstream = remote + QLatin1Char('/') + name;
            try {
                repo.setUpstream(name.toStdString(), upstream.toStdString());
                local.done << name;
            } catch (const std::exception& error) {
                local.failed << QStringLiteral("%1: %2")
                                    .arg(name, QString::fromUtf8(error.what()));
            }
        }
    });
    if (resultOut) {
        *resultOut = local;
    }
    return ok;
}


bool RepoSession::mergeBranch(const QString& name, MergeResult* resultOut, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Merge %1").arg(name), errorOut, [&](Repository& repo) {
                        const MergeResult result = repo.mergeBranch(name.toStdString());
                        if (resultOut) {
                            *resultOut = result;
                        }
                    });
}

namespace {
std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}
} // namespace

QString RepoSession::baseBranchForCurrentBranch() const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    // Repository::info(), not RepoSession::info(): the session mutex is already held
    // here, and a nested lock on a non-recursive QMutex is a hang, not an error.
    const RepoInfo repositoryInfo = m_repo->info();
    if (repositoryInfo.workDir.empty() || repositoryInfo.headBranch.empty()) {
        return {};
    }
    const std::string workDir = repositoryInfo.workDir;
    const std::string branch = repositoryInfo.headBranch;

    const auto resolves = [&workDir](const std::string& revision) {
        const ProcessResult ok =
            runGit({"rev-parse", "--verify", "--quiet", revision + "^{commit}"}, workDir);
        return ok.exitCode == 0 && !trimmed(ok.out).empty();
    };

    // The branch's own upstream is the most honest base: it is the branch this one
    // was told it follows, whatever it is named.
    const ProcessResult upstream =
        runGit({"rev-parse", "--abbrev-ref", branch + "@{upstream}"}, workDir);
    if (upstream.exitCode == 0) {
        const std::string name = trimmed(upstream.out);
        if (!name.empty() && name != branch && resolves(name)) {
            return QString::fromStdString(name);
        }
    }

    const ProcessResult originHead = runGit(
        {"symbolic-ref", "--quiet", "--short", "refs/remotes/origin/HEAD"}, workDir);
    if (originHead.exitCode == 0) {
        const std::string name = trimmed(originHead.out);
        if (!name.empty() && resolves(name)) {
            return QString::fromStdString(name);
        }
    }

    for (const char* candidate : {"origin/main", "origin/master", "main", "master"}) {
        if (resolves(candidate)) {
            return QString::fromUtf8(candidate);
        }
    }
    return {};
}

MergePreview RepoSession::previewAgainstBase(QString* baseOut, QString* errorOut) const {
    const QString base = baseBranchForCurrentBranch();
    if (baseOut) {
        *baseOut = base;
    }
    if (base.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("no base branch could be determined for this branch");
        }
        return {};
    }
    return previewMerge(QStringLiteral("HEAD"), base, errorOut);
}

MergePreview RepoSession::previewMerge(const QString& ours, const QString& theirs,
                                        QString* errorOut) const {
    MergePreview preview;
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return preview;
    }
    try {
        preview = m_repo->previewMerge(ours.toStdString(), theirs.toStdString());
    } catch (const GitError& error) {
        if (errorOut) {
            *errorOut = QString::fromStdString(error.what());
        }
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
    }
    return preview;
}

bool RepoSession::commitMerge(const QString& message, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Complete merge"), errorOut,
                    [&](Repository& repo) { repo.commitMerge(message.toStdString()); });
}

bool RepoSession::abortMerge(QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut,
                   [&](Repository& repo) { repo.abortMerge(); });
}

bool RepoSession::mergeInProgress() const {
    if (const auto cached = snapshot()) {
        return cached->merging;
    }
    QMutexLocker lock(&m_mutex);
    return m_repo && m_repo->mergeInProgress();
}

bool RepoSession::squashMerge(const QString& name, SquashResult* resultOut, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const SquashResult result = repo.squashMerge(name.toStdString());
        if (resultOut) {
            *resultOut = result;
        }
    });
}

bool RepoSession::squashInProgress() const {
    if (const auto cached = snapshot()) {
        return cached->squashing;
    }
    QMutexLocker lock(&m_mutex);
    return m_repo && m_repo->squashInProgress();
}

bool RepoSession::commitSquash(const QString& message, QString* newCommitOut, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Squash merge"), errorOut,
                    [&](Repository& repo) {
                        const Oid id = repo.commitSquash(message.toStdString());
                        if (newCommitOut) {
                            *newCommitOut = QString::fromStdString(id);
                        }
                    });
}

bool RepoSession::rebaseOnto(const QString& branch, const QString& onto, RebaseResult* resultOut,
                             QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Rebase %1").arg(branch), errorOut, [&](Repository& repo) {
                        const RebaseResult result =
                            repo.rebaseOnto(branch.toStdString(), onto.toStdString());
                        if (resultOut) {
                            *resultOut = result;
                        }
                    });
}

bool RepoSession::rebaseContinue(RebaseResult* resultOut, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Continue rebase"), errorOut,
                    [&](Repository& repo) {
                        const RebaseResult result = repo.rebaseContinue();
                        if (resultOut) {
                            *resultOut = result;
                        }
                    });
}

bool RepoSession::rebaseAbort(QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut,
                   [&](Repository& repo) { repo.rebaseAbort(); });
}

bool RepoSession::rebaseInProgress() const {
    if (const auto cached = snapshot()) {
        return cached->rebasing;
    }
    QMutexLocker lock(&m_mutex);
    return m_repo && m_repo->rebaseInProgress();
}

bool RepoSession::checkoutCommit(const QString& commitId, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Checkout commit %1").arg(commitId.left(7)), errorOut,
                    [&](Repository& repo) { repo.checkoutCommit(commitId.toStdString()); });
}

bool RepoSession::moveBranch(const QString& name, const QString& targetCommit, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Move branch %1").arg(name), errorOut, [&](Repository& repo) {
                        repo.moveBranch(name.toStdString(), targetCommit.toStdString(), false);
                    });
}

bool RepoSession::moveBranchForced(const QString& name, const QString& targetCommit,
                                   QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Move branch %1").arg(name), errorOut, [&](Repository& repo) {
                        repo.moveBranch(name.toStdString(), targetCommit.toStdString(), true);
                    });
}

bool RepoSession::canFastForward(const QString& branch, const QString& target) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return false;
    }
    try {
        return m_repo->canFastForward(branch.toStdString(), target.toStdString());
    } catch (const std::exception&) {
        // A branch or commit that cannot be resolved is simply not a
        // fast-forward as far as the UI is concerned.
        return false;
    }
}

QString RepoSession::mergeBase(const QString& a, const QString& b) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    try {
        return QString::fromStdString(m_repo->mergeBase(a.toStdString(), b.toStdString()));
    } catch (const std::exception&) {
        // An unresolvable revision has no merge base as far as the caller is concerned.
        return {};
    }
}

bool RepoSession::fastForwardBranch(const QString& branch, const QString& target,
                                    QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Fast-forward %1").arg(branch), errorOut,
                    [&](Repository& repo) {
                        repo.fastForwardBranch(branch.toStdString(), target.toStdString());
                    });
}

bool RepoSession::renameBranch(const QString& from, const QString& to, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Rename branch %1").arg(from), errorOut,
                    [&](Repository& repo) {
                        repo.renameBranch(from.toStdString(), to.toStdString());
                    });
}

bool RepoSession::cherryPick(const std::vector<QString>& commitIds, QString* newCommitOut,
                             QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Cherry-pick"), errorOut,
                    [&](Repository& repo) {
                        std::vector<Oid> ids;
                        ids.reserve(commitIds.size());
                        for (const QString& id : commitIds) {
                            ids.push_back(id.toStdString());
                        }
                        const Oid created = repo.cherryPick(ids);
                        if (newCommitOut) {
                            *newCommitOut = QString::fromStdString(created);
                        }
                    });
}

bool RepoSession::rangeIsLinear(const QString& onto, const QString& tip, QString* errorOut) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    return m_repo->rangeIsLinear(onto.toStdString(), tip.toStdString());
}

QString RepoSession::headTipId() const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    // Repository::info(), not RepoSession::info(): the session's own info() would
    // take the mutex again and deadlock.
    const RepoInfo repositoryInfo = m_repo->info();
    return QString::fromStdString(m_repo->resolveRevision(repositoryInfo.headBranch));
}

bool RepoSession::rewriteHistory(const QString& upstream, const std::vector<RewriteStep>& steps,
                                 QString* errorOut, const QString& onto, const QString& branch) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Rewrite history"), errorOut,
                    [&](Repository& repo) {
                        repo.rewriteHistory(upstream.toStdString(), steps, onto.toStdString(),
                                            branch.toStdString());
                    });
}

std::vector<RebaseCandidate> RepoSession::rebaseCandidates(const QString& upstream,
                                                           const QString& tip) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    return m_repo->rebaseCandidates(upstream.toStdString(), tip.toStdString());
}

bool RepoSession::dropCommit(const QString& commitId, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Drop commit"), errorOut,
                    [&](Repository& repo) { repo.dropCommit(commitId.toStdString()); });
}

bool RepoSession::resetTo(const QString& commitId, ResetMode mode, QString* errorOut) {
    QString modeName;
    switch (mode) {
    case ResetMode::Soft: modeName = QStringLiteral("soft"); break;
    case ResetMode::Mixed: modeName = QStringLiteral("mixed"); break;
    case ResetMode::Hard: modeName = QStringLiteral("hard"); break;
    }
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Reset --%1 to %2").arg(modeName, commitId.left(7)), errorOut,
                    [&](Repository& repo) { repo.resetTo(commitId.toStdString(), mode); });
}

bool RepoSession::revertCommit(const QString& commitId, QString* newCommitOut, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Revert"), errorOut,
                    [&](Repository& repo) {
                        const Oid created = repo.revertCommit(commitId.toStdString());
                        if (newCommitOut) {
                            *newCommitOut = QString::fromStdString(created);
                        }
                    });
}

bool RepoSession::amendHeadMessage(const QString& message, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Amend commit"), errorOut,
                    [&](Repository& repo) {
                        repo.amendHeadMessage(message.toStdString(),
                                              configBool(repo, "commit.gpgsign"));
                    });
}

bool RepoSession::amendHeadMessage(const QString& message, bool sign, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Amend commit"), errorOut,
                    [&](Repository& repo) { repo.amendHeadMessage(message.toStdString(), sign); });
}

bool RepoSession::amendHead(const QString& message, bool sign, QString* newCommitOut,
                            QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog, QStringLiteral("Amend commit"), errorOut,
                    [&](Repository& repo) {
                        const Oid id = repo.amendHead(message.toStdString(), sign);
                        if (newCommitOut) {
                            *newCommitOut = QString::fromStdString(id);
                        }
                    });
}

bool RepoSession::amendStoppedCommit(const QString& message, QString* errorOut) {
    // Not recorded: this is a step inside an open rebase, and the rebase itself is what
    // the operation log records once it finishes. Recording here would make one undo
    // rebuild the whole replayed branch.
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.amendStoppedCommit(message.toStdString());
    });
}

bool RepoSession::createTag(const QString& name, const QString& target, const QString& message,
                            QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Create tag %1").arg(name), errorOut, [&](Repository& repo) {
                        repo.createTag(name.toStdString(), target.toStdString(),
                                       message.toStdString());
                    });
}

bool RepoSession::deleteTag(const QString& name, QString* errorOut) {
    return recorded(&m_mutex, &m_snapshotEpoch, m_repo, &m_oplog,
                    QStringLiteral("Delete tag %1").arg(name), errorOut,
                    [&](Repository& repo) { repo.deleteTag(name.toStdString()); });
}

bool RepoSession::formatPatch(const QString& commitId, QString* patchOut, QString* errorOut) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        if (errorOut) {
            *errorOut = QStringLiteral("no repository is open");
        }
        return false;
    }
    try {
        const std::string patch = m_repo->formatPatch(commitId.toStdString());
        if (patchOut) {
            *patchOut = QString::fromStdString(patch);
        }
        return true;
    } catch (const std::exception& error) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(error.what());
        }
        return false;
    }
}

UpstreamInfo RepoSession::upstreamInfo(const QString& branch) const {
    if (const auto cached = branch.isEmpty() ? snapshot() : nullptr) {
        return cached->upstream;
    }
    return readOr(UpstreamInfo{}, [&](Repository& repo) {
        return repo.upstreamInfo(branch.toStdString());
    });
}

QString RepoSession::targetBranch() const {
    if (const auto cached = snapshot()) {
        return cached->targetBranch;
    }
    QMutexLocker lock(&m_mutex);
    return m_repo ? QString::fromStdString(m_repo->targetBranch()) : QString();
}

std::vector<RefMeta> RepoSession::refMeta() const {
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        return {};
    }
    return RefMetaOps(repositoryInfo.workDir).read();
}

RepoSession::WorkerSlot::WorkerSlot(RepoSession* session) : m_session(session) {}

RepoSession::WorkerSlot::~WorkerSlot() {
    bool retiredAndIdle = false;
    {
        QMutexLocker lock(&m_session->m_workerMutex);
        if (--m_session->m_workersInFlight == 0) {
            m_session->m_workersDone.wakeAll();
            retiredAndIdle = m_session->m_retired;
        }
    }
    // Queued to the session's own thread, after the finished signal this worker has
    // already posted: the receiver sees the result before the session goes.
    if (retiredAndIdle) {
        QMetaObject::invokeMethod(m_session, &QObject::deleteLater, Qt::QueuedConnection);
    }
}

void RepoSession::retire() {
    bool idle = false;
    {
        QMutexLocker lock(&m_workerMutex);
        m_retired = true;
        idle = m_workersInFlight == 0;
    }
    if (idle) {
        delete this;
    }
}

bool RepoSession::claimWorkerSlot() {
    QMutexLocker lock(&m_workerMutex);
    if (!m_alive.load() || m_retired) {
        return false;
    }
    ++m_workersInFlight;
    return true;
}

void RepoSession::requestRefMeta() {
    // Fire-and-forget on the global pool, like a history page. The subprocess must
    // not run under the session mutex: a `for-each-ref` over thousands of branches
    // takes tens of milliseconds, and holding the lock across it would stall every
    // other read.
    if (!isOpen()) {
        return;
    }
    startWorker([this] {
        if (m_alive.load()) {
            const std::string workDir = paths().workDir;
            if (!workDir.empty()) {
                RefMetaSnapshot snapshot;
                snapshot.meta = RefMetaOps(workDir).read();
                if (m_alive.load()) {
                    emit refMetaLoaded(std::move(snapshot));
                }
            }
        }
    });
}

std::vector<HookEntry> RepoSession::hooks() const {
    const RepoInfo repositoryInfo = info();
    // A bare repository has no working tree, but the hooks still live in the git
    // directory and `rev-parse --git-path hooks` answers from there.
    const std::string dir =
        repositoryInfo.workDir.empty() ? repositoryInfo.gitDir : repositoryInfo.workDir;
    if (dir.empty()) {
        return {};
    }
    return HooksOps(dir).list();
}

QString RepoSession::hooksPath() const {
    const RepoInfo repositoryInfo = info();
    const std::string dir =
        repositoryInfo.workDir.empty() ? repositoryInfo.gitDir : repositoryInfo.workDir;
    if (dir.empty()) {
        return {};
    }
    return QString::fromStdString(HooksOps(dir).dir());
}

QString RepoSession::hookContent(const QString& name) const {
    const RepoInfo repositoryInfo = info();
    const std::string dir =
        repositoryInfo.workDir.empty() ? repositoryInfo.gitDir : repositoryInfo.workDir;
    if (dir.empty()) {
        return {};
    }
    return QString::fromStdString(HooksOps(dir).read(name.toStdString()));
}

bool RepoSession::writeHook(const QString& name, const QString& content, QString* errorOut) {
    return withDirectory(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](const std::string& dir) {
        HooksOps(dir).write(name.toStdString(), content.toStdString());
    });
}

bool RepoSession::enableHook(const QString& name, QString* errorOut) {
    return withDirectory(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](const std::string& dir) {
        HooksOps(dir).enable(name.toStdString());
    });
}

bool RepoSession::disableHook(const QString& name, QString* errorOut) {
    return withDirectory(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](const std::string& dir) {
        HooksOps(dir).disable(name.toStdString());
    });
}

QString RepoSession::configValue(const QString& key) const {
    if (const auto cached = snapshot(); cached && cached->config.contains(key)) {
        return cached->config.value(key);
    }
    QMutexLocker lock(&m_mutex);
    return m_repo ? QString::fromStdString(m_repo->configValue(key.toStdString())) : QString();
}

bool RepoSession::setConfigValue(const QString& key, const QString& value, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.setConfigValue(key.toStdString(), value.toStdString());
    });
}

bool RepoSession::commitSigningEnabled() const {
    if (const auto cached = snapshot()) {
        return cached->signCommits;
    }
    QMutexLocker lock(&m_mutex);
    return m_repo ? configBool(*m_repo, "commit.gpgsign") : false;
}

CommitSignature RepoSession::commitSignature(const QString& revision) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    try {
        return m_repo->commitSignature(revision.toStdString());
    } catch (const std::exception&) {
        // An unreadable revision is not a signature; the panel shows nothing.
        return {};
    }
}

bool RepoSession::ignorePath(const QString& pattern, bool globalExclude, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.addIgnorePattern(pattern.toStdString(), globalExclude);
    });
}

QStringList RepoSession::localExcludePatterns() const {
    QStringList out;
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return out;
    }
    try {
        for (const std::string& pattern : m_repo->localExcludePatterns()) {
            out << QString::fromStdString(pattern);
        }
    } catch (const std::exception&) {
    }
    return out;
}

bool RepoSession::removeLocalExcludePattern(const QString& pattern, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.removeLocalExcludePattern(pattern.toStdString());
    });
}

QStringList RepoSession::skipWorktreePaths() const {
    QStringList out;
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return out;
    }
    try {
        for (const std::string& path : m_repo->skipWorktreePaths()) {
            out << QString::fromStdString(path);
        }
    } catch (const std::exception&) {
    }
    return out;
}

bool RepoSession::setSkipWorktree(const QStringList& paths, bool skip, QString* errorOut) {
    std::vector<std::string> list;
    for (const QString& path : paths) {
        list.push_back(path.toStdString());
    }
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut,
                   [&](Repository& repo) { repo.setSkipWorktree(list, skip); });
}

bool RepoSession::untrackPathspec(const QString& pathspec, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        repo.untrackPathspec(pathspec.toStdString());
    });
}

QString RepoSession::withCoAuthors(const QString& message,
                                   const std::vector<CoAuthor>& authors) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return message;
    }
    try {
        return QString::fromStdString(m_repo->addCoAuthors(message.toStdString(), authors));
    } catch (const std::exception&) {
        // A trailer is a convenience, not the commit; if git refuses, the message
        // the reader typed is still the one that gets committed.
        return message;
    }
}

QString RepoSession::commitTemplate() const {
    if (const auto cached = snapshot()) {
        return cached->commitTemplate;
    }
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    try {
        return QString::fromStdString(m_repo->commitTemplate());
    } catch (const std::exception&) {
        return {};
    }
}

std::vector<CoAuthor> RepoSession::recentAuthors(int limit) const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo || limit <= 0) {
        return {};
    }
    return m_repo->recentAuthors(static_cast<std::size_t>(limit));
}

QString RepoSession::externalTool(bool merge) const {
    // The config read and the tool run touch no libgit2 state, so they do not hold
    // the session mutex across the subprocess — a tool open for minutes must not
    // block every other read.
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        return {};
    }
    return QString::fromStdString(ExternalToolOps(repositoryInfo.workDir).tool(merge));
}

bool RepoSession::setExternalTool(bool merge, const QString& name, QString* errorOut) {
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("this repository has no working tree");
        }
        return false;
    }
    std::string error;
    if (!ExternalToolOps(repositoryInfo.workDir).setTool(merge, name.toStdString(), &error)) {
        if (errorOut) {
            *errorOut = QString::fromStdString(error);
        }
        return false;
    }
    return true;
}

std::vector<QString> RepoSession::availableExternalTools(bool merge) const {
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        return {};
    }
    std::vector<QString> out;
    for (const std::string& name : ExternalToolOps(repositoryInfo.workDir).availableTools(merge)) {
        out.push_back(QString::fromStdString(name));
    }
    return out;
}

SyncResult RepoSession::runExternalDiffTool(const QString& base, const QString& target,
                                            const QString& path, bool cached) const {
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        return SyncResult{false, -1, "this repository has no working tree"};
    }
    return ExternalToolOps(repositoryInfo.workDir)
        .runDiff(base.toStdString(), target.toStdString(), path.toStdString(), cached);
}

SyncResult RepoSession::runExternalMergeTool(const QString& path) const {
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        return SyncResult{false, -1, "this repository has no working tree"};
    }
    return ExternalToolOps(repositoryInfo.workDir).runMerge(path.toStdString());
}

std::vector<StashEntry> RepoSession::stashList() const {
    if (const auto cached = snapshot()) {
        return cached->stashes;
    }
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        return {};
    }
    return StashOps(repositoryInfo.workDir).list();
}

namespace {

/// Runs one stash command and turns a failure into a GitError carrying git's own
/// output. `guarded` is what locks and converts; the command is named only for the
/// case git printed nothing at all.
template <typename Fn>
bool runStash(std::unique_ptr<Repository>& repo, QMutex* mutex,
              std::atomic<quint64>* epoch, QString* errorOut,
              const char* fallback, Fn&& command) {
    return guarded(mutex, epoch, repo, errorOut, [&](Repository& repository) {
        const RepoInfo repositoryInfo = repository.info();
        if (repositoryInfo.workDir.empty()) {
            throw GitError("no working tree: this repository has no stash");
        }
        const SyncResult result = command(StashOps(repositoryInfo.workDir));
        if (!result.ok) {
            throw GitError(result.output.empty() ? std::string(fallback) : result.output);
        }
    });
}

} // namespace

bool RepoSession::stashPush(const QString& message, bool includeUntracked, const QStringList& paths,
                            QString* errorOut) {
    std::vector<std::string> target;
    target.reserve(static_cast<std::size_t>(paths.size()));
    for (const QString& path : paths) {
        target.push_back(path.toStdString());
    }
    const std::string text = message.toStdString();
    return runStash(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git stash push failed",
                    [&](const StashOps& ops) { return ops.push(text, includeUntracked, target); });
}

bool RepoSession::stashApply(const QString& stash, QString* errorOut) {
    const std::string oid = stash.toStdString();
    return runStash(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git stash apply failed",
                    [&](const StashOps& ops) { return ops.apply(oid); });
}

bool RepoSession::stashPop(const QString& stash, QString* errorOut) {
    const std::string oid = stash.toStdString();
    return runStash(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git stash pop failed",
                    [&](const StashOps& ops) { return ops.pop(oid); });
}

bool RepoSession::stashDrop(const QString& stash, QString* errorOut) {
    const std::string oid = stash.toStdString();
    return runStash(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git stash drop failed",
                    [&](const StashOps& ops) { return ops.drop(oid); });
}

bool RepoSession::stashBranch(const QString& stash, const QString& branch, QString* errorOut) {
    const std::string oid = stash.toStdString();
    const std::string name = branch.toStdString();
    return runStash(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git stash branch failed",
                    [&](const StashOps& ops) { return ops.branch(oid, name); });
}

std::vector<WorktreeEntry> RepoSession::worktrees() const {
    if (const auto cached = snapshot()) {
        return cached->worktrees;
    }
    const RepoInfo repositoryInfo = info();
    // A bare repository has no working tree of its own, but `git worktree` still
    // runs from the git directory — and listing is how its worktrees are seen.
    const std::string dir =
        repositoryInfo.workDir.empty() ? repositoryInfo.gitDir : repositoryInfo.workDir;
    if (dir.empty()) {
        return {};
    }
    return WorktreeOps(dir).list(repositoryInfo.workDir);
}

std::vector<WorktreeWip> RepoSession::worktreeWip() const {
    if (!isOpen()) {
        return {};
    }
    return worktreeWip(status());
}

std::vector<WorktreeWip> RepoSession::worktreeWip(
    const std::vector<FileChange>& currentStatus) const {
    std::vector<WorktreeWip> out;
    if (!isOpen()) {
        return out;
    }
    // The current checkout's status comes from the caller; a linked worktree is a
    // different working tree and is read below, one `git status` each.
    int currentStaged = 0;
    int currentUnstaged = 0;
    int currentConflicted = 0;
    for (const FileChange& change : currentStatus) {
        (change.staged ? currentStaged : currentUnstaged) += 1;
        if (change.kind == ChangeKind::Conflicted) {
            currentConflicted += 1;
        }
    }

    // A rebase stopped for `edit` leaves a clean tree, but the work is not done —
    // the row has to exist so the reader can see the operation and finish it.
    const bool rebaseOpen = rebaseInProgress();

    std::vector<WorktreeEntry> list = worktrees();
    std::stable_sort(list.begin(), list.end(),
                     [](const WorktreeEntry& a, const WorktreeEntry& b) {
                         return a.current && !b.current;
                     });

    const auto cached = snapshot();
    for (const WorktreeEntry& entry : list) {
        if (entry.bare) {
            continue;
        }
        WorktreeWip wip;
        if (entry.current) {
            wip.path = entry.path;
            wip.name = entry.name;
            wip.head = entry.head;
            wip.current = true;
            wip.staged = currentStaged;
            wip.unstaged = currentUnstaged;
            wip.conflicted = currentConflicted;
            wip.rebaseOpen = rebaseOpen;
            if (wip.staged == 0 && wip.unstaged == 0 && !wip.rebaseOpen) {
                continue;
            }
        } else if (cached) {
            const auto found =
                std::find_if(cached->linkedWip.begin(), cached->linkedWip.end(),
                             [&entry](const WorktreeWip& linked) { return linked.path == entry.path; });
            if (found == cached->linkedWip.end()) {
                continue;
            }
            wip = *found;
        } else if (!linkedWorktreeWip(entry, &wip)) {
            continue;
        }
        out.push_back(std::move(wip));
    }
    return out;
}

bool RepoSession::linkedWorktreeWip(const WorktreeEntry& entry, WorktreeWip* out) {
    // A prunable entry's directory is gone (or on another machine): there is no
    // working tree to ask, and spawning git there only fails.
    std::error_code missing;
    if (entry.prunable || !std::filesystem::is_directory(entry.path, missing)) {
        return false;
    }
    WorktreeWip wip;
    wip.path = entry.path;
    wip.name = entry.name;
    wip.head = entry.head;
    const ProcessResult result = runGit({"status", "--porcelain"}, entry.path);
    if (result.exitCode != 0) {
        return false;
    }
    std::istringstream stream(result.out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() < 2) {
            continue;
        }
        // XY: X is the index, Y the working tree. `??` is untracked, which
        // libgit2 reports as an unstaged change.
        const char x = line[0];
        const char y = line[1];
        if (x == '?' && y == '?') {
            wip.unstaged += 1;
            continue;
        }
        if (x != ' ') {
            wip.staged += 1;
        }
        if (y != ' ') {
            wip.unstaged += 1;
        }
        if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) {
            wip.conflicted += 1;
        }
    }
    if (wip.staged == 0 && wip.unstaged == 0) {
        return false;
    }
    *out = std::move(wip);
    return true;
}

namespace {

/// Runs one worktree command and turns a failure into a GitError carrying git's
/// own output. Like the stash helper: `guarded` locks and converts, the command
/// names itself only for the case git printed nothing.
template <typename Fn>
bool runWorktree(std::unique_ptr<Repository>& repo, QMutex* mutex,
              std::atomic<quint64>* epoch, QString* errorOut,
                 const char* fallback, Fn&& command) {
    return guarded(mutex, epoch, repo, errorOut, [&](Repository& repository) {
        const RepoInfo repositoryInfo = repository.info();
        const std::string dir =
            repositoryInfo.workDir.empty() ? repositoryInfo.gitDir : repositoryInfo.workDir;
        if (dir.empty()) {
            throw GitError("no repository directory for worktree operations");
        }
        const SyncResult result = command(WorktreeOps(dir));
        if (!result.ok) {
            throw GitError(result.output.empty() ? std::string(fallback) : result.output);
        }
    });
}

} // namespace

bool RepoSession::addWorktree(const QString& path, const QString& branch, bool createBranch,
                              const QString& startPoint, QString* errorOut) {
    const std::string target = path.toStdString();
    const std::string name = branch.toStdString();
    const std::string start = startPoint.toStdString();
    return runWorktree(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git worktree add failed",
                       [&](const WorktreeOps& ops) { return ops.add(target, name, createBranch, start); });
}

bool RepoSession::removeWorktree(const QString& path, bool force, QString* errorOut) {
    const std::string target = path.toStdString();
    return runWorktree(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git worktree remove failed",
                       [&](const WorktreeOps& ops) { return ops.remove(target, force); });
}

bool RepoSession::lockWorktree(const QString& path, const QString& reason, QString* errorOut) {
    const std::string target = path.toStdString();
    const std::string text = reason.toStdString();
    return runWorktree(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git worktree lock failed",
                       [&](const WorktreeOps& ops) { return ops.lock(target, text); });
}

bool RepoSession::unlockWorktree(const QString& path, QString* errorOut) {
    const std::string target = path.toStdString();
    return runWorktree(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git worktree unlock failed",
                       [&](const WorktreeOps& ops) { return ops.unlock(target); });
}

bool RepoSession::pruneWorktrees(QString* errorOut) {
    return runWorktree(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git worktree prune failed",
                       [&](const WorktreeOps& ops) { return ops.prune(); });
}

std::vector<SubmoduleEntry> RepoSession::submodules() const {
    if (const auto cached = snapshot()) {
        return cached->submodules;
    }
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        return {}; // a bare repository has no submodules to check out
    }
    return SubmoduleOps(repositoryInfo.workDir).list();
}

namespace {

template <typename Fn>
bool runSubmodule(std::unique_ptr<Repository>& repo, QMutex* mutex,
              std::atomic<quint64>* epoch, QString* errorOut,
                  const char* fallback, Fn&& command) {
    return guarded(mutex, epoch, repo, errorOut, [&](Repository& repository) {
        const RepoInfo repositoryInfo = repository.info();
        if (repositoryInfo.workDir.empty()) {
            throw GitError("no working tree: a bare repository has no submodules");
        }
        const SyncResult result = command(SubmoduleOps(repositoryInfo.workDir));
        if (!result.ok) {
            throw GitError(result.output.empty() ? std::string(fallback) : result.output);
        }
    });
}

} // namespace

bool RepoSession::addSubmodule(const QString& url, const QString& path, const QString& branch,
                               bool force, QString* errorOut) {
    const std::string remoteUrl = url.toStdString();
    const std::string target = path.toStdString();
    const std::string tracked = branch.toStdString();
    return runSubmodule(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git submodule add failed",
                        [&](const SubmoduleOps& ops) {
                            return ops.add(remoteUrl, target, tracked, force);
                        });
}

bool RepoSession::deinitSubmodule(const QString& path, bool force, QString* errorOut) {
    const std::string target = path.toStdString();
    return runSubmodule(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git submodule deinit failed",
                        [&](const SubmoduleOps& ops) { return ops.deinit(target, force); });
}

bool RepoSession::updateSubmodules(const QStringList& paths, bool initialize, bool remote,
                                   bool recursive, QString* errorOut) {
    std::vector<std::string> target;
    target.reserve(static_cast<std::size_t>(paths.size()));
    for (const QString& path : paths) {
        target.push_back(path.toStdString());
    }
    return runSubmodule(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git submodule update failed",
                        [&](const SubmoduleOps& ops) {
                            return ops.update(target, initialize, remote, recursive);
                        });
}

bool RepoSession::syncSubmodules(const QStringList& paths, bool recursive, QString* errorOut) {
    std::vector<std::string> target;
    target.reserve(static_cast<std::size_t>(paths.size()));
    for (const QString& path : paths) {
        target.push_back(path.toStdString());
    }
    return runSubmodule(m_repo, &m_mutex, &m_snapshotEpoch, errorOut, "git submodule sync failed",
                        [&](const SubmoduleOps& ops) { return ops.sync(target, recursive); });
}

void RepoSession::requestAutoUpdateSubmodules(bool remote) {
    startWorker([this, remote] {
        if (!m_alive.load()) {
            return;
        }
        QString error;
        const bool ok =
            updateSubmodules({}, /*initialize=*/true, remote, /*recursive=*/true, &error);
        if (!m_alive.load()) {
            return;
        }
        emit submodulesUpdated(ok, error);
    });
}

std::vector<RemoteInfo> RepoSession::remotes() const {
    if (const auto cached = snapshot()) {
        return cached->remotes;
    }
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        return {};
    }
    return RemoteOps(repositoryInfo.workDir).remotes();
}

SyncResult RepoSession::syncBlocking(SyncKind kind, const QString& remote,
                                     const QString& branch, PullMode mode) const {
    // The binary moves refs and objects behind libgit2's back.
    const RetireSnapshotOnExit retire{this};
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        SyncResult result;
        result.output = "no working tree: cannot talk to a remote from a bare repository";
        return result;
    }

    const RemoteOps ops(repositoryInfo.workDir);
    switch (kind) {
    case SyncKind::Fetch:
        return ops.fetch(remote.toStdString(), true);
    case SyncKind::FetchAll:
        return ops.fetchAll(true);
    case SyncKind::Pull:
        return ops.pull(remote.toStdString(), branch.toStdString(), mode);
    case SyncKind::Push: {
        // Push without an upstream should also configure tracking, so the
        // ahead/behind counters start working on the next refresh.
        const UpstreamInfo upstream = upstreamInfo();
        return ops.push(remote.toStdString(), branch.toStdString(), !upstream.configured);
    }
    case SyncKind::PushForce:
        // A force push rewrites a branch that already exists; there is nothing to
        // configure tracking for, and setUpstream would be a lie.
        return ops.push(remote.toStdString(), branch.toStdString(), /*setUpstream=*/false,
                        PushMode::ForceWithLease);
    case SyncKind::PushTags:
        return ops.pushTags(remote.toStdString());
    }
    return SyncResult{};
}

SyncResult RepoSession::fetchBlocking(const QStringList& remotes, bool prune) const {
    const RetireSnapshotOnExit retire{this};
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        SyncResult result;
        result.output = "no working tree: cannot talk to a remote from a bare repository";
        return result;
    }

    if (remotes.isEmpty()) {
        return RemoteOps(repositoryInfo.workDir).fetchAll(prune);
    }

    // A subset: one `git fetch` per remote, aggregated. The first failure is kept
    // as the exit code, but every remote is still tried — a broken `backup` must
    // not stop `origin` from being fetched.
    SyncResult aggregate;
    aggregate.ok = true;
    aggregate.exitCode = 0;
    for (const QString& remote : remotes) {
        const SyncResult one =
            RemoteOps(repositoryInfo.workDir).fetch(remote.toStdString(), prune);
        if (!one.output.empty()) {
            aggregate.output += one.output;
            if (aggregate.output.back() != '\n') {
                aggregate.output += '\n';
            }
        }
        if (!one.ok) {
            aggregate.ok = false;
            aggregate.exitCode = one.exitCode;
        }
    }
    return aggregate;
}

void RepoSession::requestSync(SyncKind kind, const QString& remote, const QString& branch,
                              PullMode mode) {
    startWorker([this, kind, remote, branch, mode] {
        if (!m_alive.load()) {
            return;
        }

        const SyncResult result = syncBlocking(kind, remote, branch, mode);
        if (!m_alive.load()) {
            return;
        }

        QString summary;
        switch (kind) {
        case SyncKind::Fetch: summary = QStringLiteral("fetch"); break;
        case SyncKind::FetchAll: summary = QStringLiteral("fetch all"); break;
        case SyncKind::Pull: summary = QStringLiteral("pull"); break;
        case SyncKind::Push: summary = QStringLiteral("push"); break;
        case SyncKind::PushForce: summary = QStringLiteral("force push"); break;
        case SyncKind::PushTags: summary = QStringLiteral("push tags"); break;
        }

        emit syncFinished(result.ok, summary, QString::fromStdString(result.output));
    });
}

void RepoSession::requestFetch(const QStringList& remotes, bool prune) {
    startWorker([this, remotes, prune] {
        if (!m_alive.load()) {
            return;
        }
        const SyncResult result = fetchBlocking(remotes, prune);
        if (!m_alive.load()) {
            return;
        }
        emit syncFinished(result.ok, QStringLiteral("auto-fetch"),
                          QString::fromStdString(result.output));
    });
}

SyncResult RepoSession::pushTagBlocking(const QString& remote, const QString& tag) const {
    const RetireSnapshotOnExit retire{this};
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        SyncResult result;
        result.output = "no working tree: cannot talk to a remote from a bare repository";
        return result;
    }
    return RemoteOps(repositoryInfo.workDir)
        .pushTag(remote.toStdString(), tag.toStdString());
}

SyncResult RepoSession::deleteRemoteTagBlocking(const QString& remote, const QString& tag) const {
    const RetireSnapshotOnExit retire{this};
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        SyncResult result;
        result.output = "no working tree: cannot talk to a remote from a bare repository";
        return result;
    }
    return RemoteOps(repositoryInfo.workDir)
        .deleteRemoteTag(remote.toStdString(), tag.toStdString());
}

void RepoSession::requestPushTag(const QString& remote, const QString& tag) {
    startWorker([this, remote, tag] {
        if (!m_alive.load()) {
            return;
        }
        const SyncResult result = pushTagBlocking(remote, tag);
        if (!m_alive.load()) {
            return;
        }
        emit syncFinished(result.ok, QStringLiteral("push tag"),
                          QString::fromStdString(result.output));
    });
}

bool RepoSession::applyPatch(const QString& path, bool* mailboxOut, QString* errorOut) {
    // Read before the lock: this only opens the file, and the caller wants the
    // answer even if the repository is gone.
    if (mailboxOut) {
        *mailboxOut = PatchOps::isMailbox(path.toStdString());
    }
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const RepoInfo repositoryInfo = repo.info();
        const std::string dir =
            repositoryInfo.workDir.empty() ? repositoryInfo.gitDir : repositoryInfo.workDir;
        if (dir.empty()) {
            throw GitError("no repository directory to apply the patch in");
        }
        const SyncResult result = PatchOps(dir).apply(path.toStdString());
        if (!result.ok) {
            throw GitError(result.output.empty() ? std::string("git refused the patch")
                                                 : result.output);
        }
    });
}

bool RepoSession::sparseCheckoutEnabled() const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return false;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    if (dir.empty()) {
        return false;
    }
    return SparseOps(dir).enabled();
}

QStringList RepoSession::sparseCheckoutDirectories() const {
    QMutexLocker lock(&m_mutex);
    QStringList out;
    if (!m_repo) {
        return out;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    if (dir.empty()) {
        return out;
    }
    for (const std::string& entry : SparseOps(dir).list()) {
        out << QString::fromStdString(entry);
    }
    return out;
}

QStringList RepoSession::sparseCheckoutTopLevelDirectories() const {
    QMutexLocker lock(&m_mutex);
    QStringList out;
    if (!m_repo) {
        return out;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    if (dir.empty()) {
        return out;
    }
    for (const std::string& entry : SparseOps(dir).topLevelDirectories()) {
        out << QString::fromStdString(entry);
    }
    return out;
}

bool RepoSession::setSparseCheckout(const QStringList& dirs, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const RepoInfo info = repo.info();
        const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
        if (dir.empty()) {
            throw GitError("this repository has no working tree to check out sparsely");
        }
        std::vector<std::string> patterns;
        patterns.reserve(static_cast<std::size_t>(dirs.size()));
        for (const QString& entry : dirs) {
            patterns.push_back(entry.toStdString());
        }
        const SyncResult result = SparseOps(dir).set(patterns);
        if (!result.ok) {
            // git's own message, not a paraphrase: it names the file it choked on.
            throw GitError(result.output.empty() ? "sparse-checkout failed" : result.output);
        }
    });
}

bool RepoSession::gitflowInitialized() const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return false;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    return !dir.empty() && GitFlowOps(dir).initialized();
}

QStringList RepoSession::gitflowSummary() const {
    QMutexLocker lock(&m_mutex);
    QStringList out;
    if (!m_repo) {
        return out;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    if (dir.empty()) {
        return out;
    }
    const GitFlowConfig cfg = GitFlowOps(dir).config();
    out << QStringLiteral("master: %1").arg(QString::fromStdString(cfg.master));
    out << QStringLiteral("develop: %1").arg(QString::fromStdString(cfg.develop));
    out << QStringLiteral("feature: %1").arg(QString::fromStdString(cfg.prefixFeature));
    out << QStringLiteral("release: %1").arg(QString::fromStdString(cfg.prefixRelease));
    out << QStringLiteral("hotfix: %1").arg(QString::fromStdString(cfg.prefixHotfix));
    out << QStringLiteral("bugfix: %1").arg(QString::fromStdString(cfg.prefixBugfix));
    return out;
}

bool RepoSession::initGitflow(const QString& master, const QString& develop, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const RepoInfo info = repo.info();
        const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
        if (dir.empty()) {
            throw GitError("this repository has no working tree for the gitflow workflow");
        }
        GitFlowConfig cfg;
        if (!master.trimmed().isEmpty()) {
            cfg.master = master.trimmed().toStdString();
        }
        if (!develop.trimmed().isEmpty()) {
            cfg.develop = develop.trimmed().toStdString();
        }
        const SyncResult result = GitFlowOps(dir).init(cfg);
        if (!result.ok) {
            throw GitError(result.output.empty() ? "gitflow init failed" : result.output);
        }
    });
}

bool RepoSession::gitflowStart(const QString& kind, const QString& name, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const RepoInfo info = repo.info();
        const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
        GitFlowKind parsed = GitFlowKind::Feature;
        if (dir.empty() || !gitFlowKindFromName(kind.toStdString(), &parsed)) {
            throw GitError("unknown gitflow branch kind '" + kind.toStdString() + "'");
        }
        const SyncResult result = GitFlowOps(dir).start(parsed, name.trimmed().toStdString());
        if (!result.ok) {
            throw GitError(result.output.empty() ? "gitflow start failed" : result.output);
        }
    });
}

bool RepoSession::gitflowFinish(const QString& kind, const QString& name, const QString& version,
                                QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const RepoInfo info = repo.info();
        const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
        GitFlowKind parsed = GitFlowKind::Feature;
        if (dir.empty() || !gitFlowKindFromName(kind.toStdString(), &parsed)) {
            throw GitError("unknown gitflow branch kind '" + kind.toStdString() + "'");
        }
        const SyncResult result =
            GitFlowOps(dir).finish(parsed, name.trimmed().toStdString(), version.trimmed().toStdString());
        if (!result.ok) {
            throw GitError(result.output.empty() ? "gitflow finish failed" : result.output);
        }
    });
}

bool RepoSession::lfsAvailable() const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return false;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    return !dir.empty() && GitLfsOps(dir).available();
}

QString RepoSession::lfsVersion() const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    return dir.empty() ? QString() : QString::fromStdString(GitLfsOps(dir).version());
}

bool RepoSession::lfsConfigured() const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return false;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    return !dir.empty() && GitLfsOps(dir).configured();
}

QStringList RepoSession::lfsTrackedPatterns() const {
    QMutexLocker lock(&m_mutex);
    QStringList out;
    if (!m_repo) {
        return out;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    if (dir.empty()) {
        return out;
    }
    for (const LfsPattern& entry : GitLfsOps(dir).trackedPatterns()) {
        out << (entry.source.empty()
                    ? QString::fromStdString(entry.pattern)
                    : QStringLiteral("%1 (%2)").arg(QString::fromStdString(entry.pattern),
                                                    QString::fromStdString(entry.source)));
    }
    return out;
}

QStringList RepoSession::lfsFiles() const {
    QMutexLocker lock(&m_mutex);
    QStringList out;
    if (!m_repo) {
        return out;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    if (dir.empty()) {
        return out;
    }
    for (const LfsFile& file : GitLfsOps(dir).files()) {
        out << QStringLiteral("%1 %2 %3")
                   .arg(file.objectPresent ? QStringLiteral("*") : QStringLiteral("-"),
                        QString::fromStdString(file.oid), QString::fromStdString(file.path));
    }
    return out;
}

QString RepoSession::lfsStatusReport() const {
    QMutexLocker lock(&m_mutex);
    if (!m_repo) {
        return {};
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    return dir.empty() ? QString() : QString::fromStdString(GitLfsOps(dir).statusReport());
}

bool RepoSession::lfsInstall(QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [](Repository& repo) {
        const RepoInfo info = repo.info();
        const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
        if (dir.empty()) {
            throw GitError("this repository has no working tree for LFS");
        }
        const SyncResult result = GitLfsOps(dir).install();
        if (!result.ok) {
            throw GitError(result.output.empty() ? "git lfs install failed" : result.output);
        }
    });
}

bool RepoSession::lfsTrack(const QString& pattern, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const RepoInfo info = repo.info();
        const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
        if (dir.empty() || pattern.trimmed().isEmpty()) {
            throw GitError("a pattern is required to track");
        }
        const SyncResult result = GitLfsOps(dir).track(pattern.trimmed().toStdString());
        if (!result.ok) {
            throw GitError(result.output.empty() ? "git lfs track failed" : result.output);
        }
    });
}

bool RepoSession::lfsUntrack(const QString& pattern, QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [&](Repository& repo) {
        const RepoInfo info = repo.info();
        const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
        if (dir.empty() || pattern.trimmed().isEmpty()) {
            throw GitError("a pattern is required to untrack");
        }
        const SyncResult result = GitLfsOps(dir).untrack(pattern.trimmed().toStdString());
        if (!result.ok) {
            throw GitError(result.output.empty() ? "git lfs untrack failed" : result.output);
        }
    });
}

bool RepoSession::lfsCheckout(QString* errorOut) {
    return guarded(&m_mutex, &m_snapshotEpoch, m_repo, errorOut, [](Repository& repo) {
        const RepoInfo info = repo.info();
        const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
        if (dir.empty()) {
            throw GitError("this repository has no working tree for LFS");
        }
        const SyncResult result = GitLfsOps(dir).checkout();
        if (!result.ok) {
            throw GitError(result.output.empty() ? "git lfs checkout failed" : result.output);
        }
    });
}

SyncResult RepoSession::lfsBlocking(LfsTask task, const QString& remote, const QString& ref) const {
    const RetireSnapshotOnExit retire{this};
    QMutexLocker lock(&m_mutex);
    SyncResult result;
    if (!m_repo) {
        result.output = "no repository is open";
        return result;
    }
    const RepoInfo info = m_repo->info();
    const std::string dir = info.workDir.empty() ? info.gitDir : info.workDir;
    if (dir.empty()) {
        result.output = "no repository directory for LFS";
        return result;
    }
    const GitLfsOps ops(dir);
    const std::string remoteName = remote.trimmed().toStdString();
    switch (task) {
    case LfsTask::Fetch:
        return ops.fetch(remoteName);
    case LfsTask::Pull:
        return ops.pull(remoteName);
    case LfsTask::Push:
        return ops.push(remoteName, ref.trimmed().toStdString());
    case LfsTask::PushAll:
        return ops.pushAll(remoteName);
    case LfsTask::Prune:
        return ops.prune(false);
    case LfsTask::PruneDryRun:
        return ops.prune(true);
    case LfsTask::Checkout:
        return ops.checkout();
    }
    return result;
}

void RepoSession::requestLfs(LfsTask task, const QString& remote, const QString& ref) {
    startWorker([this, task, remote, ref] {
        if (!m_alive.load()) {
            return;
        }
        const SyncResult result = lfsBlocking(task, remote, ref);
        if (!m_alive.load()) {
            return;
        }
        emit lfsFinished(result.ok, QString::fromLatin1(lfsTaskName(task)),
                         QString::fromStdString(result.output));
    });
}

SyncResult RepoSession::maintenanceBlocking(MaintenanceTask task) const {
    const RetireSnapshotOnExit retire{this};
    QMutexLocker lock(&m_mutex);
    SyncResult result;
    if (!m_repo) {
        result.output = "no repository is open";
        return result;
    }
    const RepoInfo repositoryInfo = m_repo->info();
    const std::string dir =
        repositoryInfo.workDir.empty() ? repositoryInfo.gitDir : repositoryInfo.workDir;
    if (dir.empty()) {
        result.output = "no repository directory for maintenance";
        return result;
    }
    return MaintenanceOps(dir).run(task);
}

void RepoSession::requestMaintenance(MaintenanceTask task) {
    startWorker([this, task] {
        if (!m_alive.load()) {
            return;
        }
        const SyncResult result = maintenanceBlocking(task);
        if (!m_alive.load()) {
            return;
        }
        const QString name =
            task == MaintenanceTask::Gc ? QStringLiteral("gc") : QStringLiteral("fsck");
        emit maintenanceFinished(result.ok, name, QString::fromStdString(result.output));
    });
}

std::vector<CommitInfo> RepoSession::commitsBlocking(quint64 skip, quint32 limit, bool* hasMore,
                                                     const HistoryFilter& filter) const {
    return readOr(std::vector<CommitInfo>{}, [&](Repository& repo) {
        return repo.commits(skip, limit, hasMore, filter);
    });
}

void RepoSession::invalidateCommitWalk() const {
    m_walkStale.store(true);
}

std::unordered_map<std::string, std::vector<RefInfo>> RepoSession::refsByCommit(
    const HistoryFilter& filter) const {
    if (const auto cached = snapshot()) {
        if (filter.visibleRefs.empty()) {
            return cached->refsByCommit;
        }
        const std::unordered_set<std::string> visible(filter.visibleRefs.begin(),
                                                      filter.visibleRefs.end());
        std::unordered_map<std::string, std::vector<RefInfo>> out;
        for (const auto& [commit, bucket] : cached->refsByCommit) {
            for (const RefInfo& ref : bucket) {
                if (visible.count(ref.fullName) > 0) {
                    out[commit].push_back(ref);
                }
            }
        }
        return out;
    }
    return readOr(std::unordered_map<std::string, std::vector<RefInfo>>{}, [&](Repository& repo) {
        return repo.refsByCommit(filter);
    });
}

void RepoSession::requestCommits(quint64 generation, quint64 skip, quint32 limit,
                                 const HistoryFilter& filter) {
    startWorker([this, generation, skip, limit, filter] {
        QVector<CommitInfo> page;
        bool hasMore = false;
        QString error;
        std::size_t readOutsideGraph = 0;
        std::string dir;
        {
            QMutexLocker lock(&m_mutex);
            if (!m_repo || !m_alive.load()) {
                return;
            }
            if (m_walkStale.exchange(false)) {
                m_repo->invalidateCommitWalk();
            }
            try {
                const auto rows = m_repo->commits(skip, limit, &hasMore, filter);
                page.reserve(static_cast<int>(rows.size()));
                for (const auto& row : rows) {
                    page.push_back(row);
                }
                if (m_autoCommitGraph.load() && m_repo->commitGraphEnabled()) {
                    readOutsideGraph = m_repo->walkReadOutsideGraph();
                    const RepoInfo repositoryInfo = m_repo->info();
                    dir = repositoryInfo.workDir.empty() ? repositoryInfo.gitDir
                                                         : repositoryInfo.workDir;
                }
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
            }
        }
        if (!m_alive.load()) {
            return;
        }
        CommitPage result;
        result.generation = generation;
        result.skip = skip;
        result.commits = std::move(page);
        result.hasMore = hasMore;
        result.error = error;
        emit commitsLoaded(result);
        indexHistoryIfWorthIt(readOutsideGraph, dir);
    });
}

void RepoSession::indexHistoryIfWorthIt(std::size_t readOutsideGraph, const std::string& dir) {
    // Below this the walk reads the missing commits faster than git would write
    // them; above it, every reopen pays for the whole history again.
    constexpr std::size_t kWorthIndexing = 10000;
    if (readOutsideGraph < kWorthIndexing || dir.empty() ||
        m_commitGraphWriteStarted.exchange(true)) {
        return;
    }
    // No session lock: git writes the graph beside the objects and renames it into
    // place, and the walk in use keeps reading the file it already mapped. `--split`
    // makes the next one cost only the commits since.
    startWorker([dir] { runGit({"commit-graph", "write", "--reachable", "--split"}, dir); });
}

std::vector<CommitInfo> RepoSession::searchCommitsBlocking(const QString& query,
                                                           CommitSearchField field, quint32 limit,
                                                           std::size_t skip) const {
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        return {};
    }
    return Repository::searchCommitsIn(repositoryInfo.workDir, query.toStdString(), field, skip,
                                       limit);
}

void RepoSession::requestCommitSearch(const QString& query, CommitSearchField field, quint32 limit) {
    startWorker([this, query, field, limit] {
        std::vector<CommitInfo> rows;
        QString error;
        // Not under the session mutex: the search is a git subprocess that can run for
        // seconds, and every read the window makes would queue behind it.
        const RepoInfo repositoryInfo = info();
        if (repositoryInfo.workDir.empty() || !m_alive.load()) {
            return;
        }
        try {
            rows = Repository::searchCommitsIn(repositoryInfo.workDir, query.toStdString(),
                                               field, 0, limit);
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        if (!m_alive.load()) {
            return;
        }
        if (!error.isEmpty()) {
            emit requestFailed(error);
            return;
        }
        CommitSearchResults result;
        result.query = query;
        result.field = static_cast<int>(field);
        result.commits.reserve(static_cast<int>(rows.size()));
        for (const CommitInfo& row : rows) {
            result.commits.push_back(row);
        }
        emit commitSearchFinished(result);
    });
}

QStringList RepoSession::ignoredPaths(const QString& workDir) {
    QStringList out;
    try {
        for (const std::string& path : Repository::ignoredPaths(workDir.toStdString())) {
            out << QString::fromStdString(path);
        }
    } catch (const std::exception&) {
        // A failed spawn is "nothing known to be ignored": the watcher watches more.
    }
    return out;
}

} // namespace gk
