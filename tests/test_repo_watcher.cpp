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
    void aFileEditedInPlaceIsReported();
    void aDirectoryCreatedLaterIsWatched();
    void whatGitIgnoresIsNotReported();
    void aMovedRefIsGitState();

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
    QVERIFY(QDir().mkpath(m_work + QStringLiteral("/.git/refs/heads")));
    QVERIFY(QDir().mkpath(m_work + QStringLiteral("/deep/er/still")));
    QVERIFY(QDir().mkpath(m_work + QStringLiteral("/build/out")));
    QVERIFY(touch(m_work + QStringLiteral("/deep/er/still/file.txt")));
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
    QTRY_VERIFY(watcher.ready());
    QVERIFY(watcher.watchedDirectoryCount() > 0);
    QSignalSpy spy(&watcher, &RepoWatcher::changed);
    // FSEvents (macOS) delivers late the fixture's own writes made before watching began.
    QTest::qWait(700);
    spy.clear();

    QVERIFY(touch(m_work + QStringLiteral("/a.txt")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);

    const int settled = spy.count();
    QTest::qWait(700);
    QCOMPARE(spy.count(), settled);
}

void RepoWatcherTest::aNestedChangeIsReported() {
    RepoWatcher watcher;
    watcher.setRoots(m_work, m_work + QStringLiteral("/.git"));
    QTRY_VERIFY(watcher.ready());
    QSignalSpy spy(&watcher, &RepoWatcher::changed);

    QVERIFY(touch(m_work + QStringLiteral("/src/nested.txt")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
}

void RepoWatcherTest::stoppingRemovesEveryWatch() {
    RepoWatcher watcher;
    watcher.setRoots(m_work, m_work + QStringLiteral("/.git"));
    QTRY_VERIFY(watcher.ready());
    QVERIFY(watcher.watchedDirectoryCount() > 0);

    watcher.stop();
    QCOMPARE(watcher.watchedDirectoryCount(), 0);
}

void RepoWatcherTest::aFileEditedInPlaceIsReported() {
    RepoWatcher watcher;
    watcher.setRoots(m_work, m_work + QStringLiteral("/.git"));
    QTRY_VERIFY(watcher.ready());
    QSignalSpy spy(&watcher, &RepoWatcher::changed);
    // FSEvents (macOS) delivers late the fixture's own writes made before watching began.
    QTest::qWait(800);
    spy.clear();

    // Rewritten where it is, three levels down: no entry appears or goes, only the
    // content changes — what an editor saving in place does.
    QFile file(m_work + QStringLiteral("/deep/er/still/file.txt"));
    QVERIFY(file.open(QIODevice::ReadWrite));
    file.seek(0);
    file.write("edited");
    file.close();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
    QCOMPARE(spy.last().at(0).toBool(), false);
}

void RepoWatcherTest::aDirectoryCreatedLaterIsWatched() {
    RepoWatcher watcher;
    watcher.setRoots(m_work, m_work + QStringLiteral("/.git"));
    QTRY_VERIFY(watcher.ready());
    QSignalSpy spy(&watcher, &RepoWatcher::changed);

    QVERIFY(QDir().mkpath(m_work + QStringLiteral("/fresh/sub")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
    QTest::qWait(500);
    spy.clear();

    QVERIFY(touch(m_work + QStringLiteral("/fresh/sub/new.txt")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
}

void RepoWatcherTest::whatGitIgnoresIsNotReported() {
    RepoWatcher watcher;
    watcher.setIgnoredLister([](const QString&) { return QStringList{QStringLiteral("build/")}; });
    watcher.setRoots(m_work, m_work + QStringLiteral("/.git"));
    QTRY_VERIFY(watcher.ready());
    QSignalSpy spy(&watcher, &RepoWatcher::changed);
    // FSEvents (macOS) delivers late the fixture's own writes made before watching began.
    QTest::qWait(800);
    spy.clear();

    QVERIFY(touch(m_work + QStringLiteral("/build/out/artifact.o")));
    QVERIFY(touch(m_work + QStringLiteral("/build/log.txt")));
    QTest::qWait(1200);
    QCOMPARE(spy.count(), 0);

    // The control: the same write outside the ignored directory is reported.
    QVERIFY(touch(m_work + QStringLiteral("/src/kept.txt")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
}

void RepoWatcherTest::aMovedRefIsGitState() {
    RepoWatcher watcher;
    watcher.setRoots(m_work, m_work + QStringLiteral("/.git"));
    QTRY_VERIFY(watcher.ready());
    QSignalSpy spy(&watcher, &RepoWatcher::changed);
    // FSEvents (macOS) delivers late the fixture's own writes made before watching began.
    QTest::qWait(800);
    spy.clear();

    QVERIFY(touch(m_work + QStringLiteral("/.git/refs/heads/topic")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
    QCOMPARE(spy.last().at(0).toBool(), true);
}

QTEST_MAIN(RepoWatcherTest)
#include "test_repo_watcher.moc"
