#include "ui/repo_watcher.h"

#include <QDir>
#include <QFileInfo>

namespace gk {
namespace {

constexpr int kSettleMs = 400;
constexpr int kMaxDirectories = 512;

} // namespace

RepoWatcher::RepoWatcher(QObject* parent) : QObject(parent) {
    m_settle.setSingleShot(true);
    m_settle.setInterval(kSettleMs);
    connect(&m_settle, &QTimer::timeout, this, &RepoWatcher::onSettled);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
            &RepoWatcher::onDirectoryChanged);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &RepoWatcher::onFileChanged);
}

void RepoWatcher::setRoots(const QString& workDir, const QString& gitDir) {
    stop();
    m_workDir = workDir;
    m_gitDir = gitDir;
    rewatch();
}

void RepoWatcher::stop() {
    m_settle.stop();
    const QStringList paths = m_watcher.directories() + m_watcher.files();
    if (!paths.isEmpty()) {
        m_watcher.removePaths(paths);
    }
    m_workDir.clear();
    m_gitDir.clear();
}

int RepoWatcher::watchedDirectoryCount() const {
    return m_watcher.directories().size();
}

void RepoWatcher::collect(const QString& root, QStringList* out) const {
    QStringList pending{root};
    while (!pending.isEmpty() && out->size() < kMaxDirectories) {
        const QString dir = pending.takeFirst();
        out->append(dir);
        const QFileInfoList children =
            QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& child : children) {
            if (child.fileName() == QStringLiteral(".git")) {
                continue;
            }
            if (out->size() + pending.size() >= kMaxDirectories) {
                break;
            }
            pending.append(child.absoluteFilePath());
        }
    }
}

void RepoWatcher::rewatch() {
    if (m_workDir.isEmpty() && m_gitDir.isEmpty()) {
        return;
    }

    QStringList wanted;
    if (!m_workDir.isEmpty()) {
        collect(m_workDir, &wanted);
    }
    if (!m_gitDir.isEmpty()) {
        wanted << m_gitDir;
        const QString refs = m_gitDir + QStringLiteral("/refs");
        if (QFileInfo(refs).isDir()) {
            collect(refs, &wanted);
        }
    }

    const QStringList watchedDirs = m_watcher.directories();
    QStringList newDirs;
    for (const QString& dir : wanted) {
        if (!watchedDirs.contains(dir)) {
            newDirs << dir;
        }
    }
    if (!newDirs.isEmpty()) {
        m_watcher.addPaths(newDirs);
    }

    const QStringList watchedFiles = m_watcher.files();
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
        m_watcher.addPaths(newFiles);
    }
}

void RepoWatcher::onDirectoryChanged(const QString&) {
    rewatch();
    schedule();
}

void RepoWatcher::onFileChanged(const QString&) {
    schedule();
}

void RepoWatcher::schedule() {
    m_settle.start();
}

void RepoWatcher::onSettled() {
    emit changed();
}

} // namespace gk
