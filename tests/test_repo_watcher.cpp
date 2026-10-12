#include "ui/repo_watcher.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

class RepoWatcherTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aNewFileIsReportedOnce();
    void aNestedChangeIsReported();
    void stoppingRemovesEveryWatch();

private:
    bool touch(const QString& path);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_work;
};

void RepoWatcherTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_work = QDir(m_dir->path()).filePath(QStringLiteral("work"));
    QVERIFY(QDir().mkpath(m_work + QStringLiteral("/src")));
    QVERIFY(QDir().mkpath(m_work + QStringLiteral("/.git")));
    QVERIFY(touch(m_work + QStringLiteral("/.git/HEAD")));
    QVERIFY(touch(m_work + QStringLiteral("/.git/index")));
}

void RepoWatcherTest::cleanup() {
    m_dir.reset();
}

bool RepoWatcherTest::touch(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write("x");
    return true;
}

void RepoWatcherTest::aNewFileIsReportedOnce() {
    RepoWatcher watcher;
    watcher.setRoots(m_work, m_work + QStringLiteral("/.git"));
    QVERIFY(watcher.watchedDirectoryCount() > 0);
    QSignalSpy spy(&watcher, &RepoWatcher::changed);

    QVERIFY(touch(m_work + QStringLiteral("/a.txt")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);

    const int settled = spy.count();
    QTest::qWait(700);
    QCOMPARE(spy.count(), settled);
}

void RepoWatcherTest::aNestedChangeIsReported() {
    RepoWatcher watcher;
    watcher.setRoots(m_work, m_work + QStringLiteral("/.git"));
    QSignalSpy spy(&watcher, &RepoWatcher::changed);

    QVERIFY(touch(m_work + QStringLiteral("/src/nested.txt")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
}

void RepoWatcherTest::stoppingRemovesEveryWatch() {
    RepoWatcher watcher;
    watcher.setRoots(m_work, m_work + QStringLiteral("/.git"));
    QVERIFY(watcher.watchedDirectoryCount() > 0);

    watcher.stop();
    QCOMPARE(watcher.watchedDirectoryCount(), 0);
}

QTEST_MAIN(RepoWatcherTest)
#include "test_repo_watcher.moc"
