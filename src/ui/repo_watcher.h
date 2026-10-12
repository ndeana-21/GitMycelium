#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace gk {

/// Reports that a repository's directories changed, once, after the changes settle.
///
/// QFileSystemWatcher is not recursive and says nothing about a file's content
/// changing, so the directories are watched explicitly and that is all the signal
/// there is: libgit2 1.9 has no fsmonitor, and the cap keeps a monorepo from
/// exhausting the inotify budget.
class RepoWatcher : public QObject {
    Q_OBJECT

public:
    explicit RepoWatcher(QObject* parent = nullptr);

    void setRoots(const QString& workDir, const QString& gitDir);
    void stop();

    int watchedDirectoryCount() const;

signals:
    void changed();

private:
    void rewatch();
    void schedule();
    void collect(const QString& root, QStringList* out) const;
    void onDirectoryChanged(const QString& path);
    void onFileChanged(const QString& path);
    void onSettled();

    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    QString m_workDir;
    QString m_gitDir;
};

} // namespace gk
