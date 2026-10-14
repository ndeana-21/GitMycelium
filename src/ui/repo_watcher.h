#pragma once

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

namespace gk {

/// Reports that a repository changed on disk, once, after the changes settle: the
/// work tree (a file saved, created, removed) or git's own state (refs, HEAD, the
/// index, an operation's markers).
///
/// The whole work tree is watched, file contents included: inotify on Linux (every
/// directory, added on a worker), FSEvents on macOS (recursive by itself). What git
/// ignores is left out, so a build directory does not keep the window refreshing.
/// Elsewhere it falls back to QFileSystemWatcher, which sees entries come and go in a
/// capped set of directories and the git dir's `index`/`HEAD`.
class RepoWatcher : public QObject {
    Q_OBJECT

public:
    /// Answers what git ignores in a work tree, relative paths, directories ending
    /// in '/'. Called on a worker thread.
    using IgnoredLister = std::function<QStringList(const QString& workDir)>;

    explicit RepoWatcher(QObject* parent = nullptr);
    ~RepoWatcher() override;

    void setIgnoredLister(IgnoredLister lister);
    /// `commonDir` is where refs live for a linked worktree; empty means `gitDir`.
    void setRoots(const QString& workDir, const QString& gitDir,
                  const QString& commonDir = QString());
    void stop();

    int watchedDirectoryCount() const;
    /// True once every directory is being watched (the worker finished its scan).
    bool ready() const;

signals:
    /// `gitState`: refs, HEAD or the index moved, not just files in the work tree.
    void changed(bool gitState);

private:
    struct Native;

    void noteChange(bool gitState);
    void onSettled();
    bool startNative();
    void stopNative();
    int nativeDirectoryCount() const;
    bool nativeReady() const;
    void startFallback();
    void rewatchFallback();
    void collectFallback(const QString& root, QStringList* out) const;
    bool isGitPath(const QString& path) const;

    std::unique_ptr<Native> m_native;
    QFileSystemWatcher m_fallback;
    bool m_usingFallback = false;
    IgnoredLister m_ignoredLister;
    QTimer m_settle;
    QElapsedTimer m_firstPending;
    bool m_pendingGitState = false;
    QString m_workDir;
    QString m_gitDir;
    QString m_commonDir;
};

} // namespace gk
