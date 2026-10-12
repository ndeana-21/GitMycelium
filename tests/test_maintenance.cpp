#include "git/cli.h"
#include "git/maintenance.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).exitCode == 0;
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

const QStringList kIdentity{"-c", "user.name=Test", "-c", "user.email=test@localhost"};

int packCount(const QString& repo) {
    return QDir(QDir(repo).filePath(QStringLiteral(".git/objects/pack")))
        .entryList(QStringList{QStringLiteral("*.pack")}, QDir::Files)
        .size();
}

} // namespace

class MaintenanceTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void taskNamesRoundTrip();
    void commitGraphIsWrittenAndVerifies();
    void multiPackIndexIsWrittenAndVerifies();
    void optimizeWritesBothAndIsRepeatable();
    void maintenanceRunSucceeds();
    void aTaskWithNothingToDoReportsGitsRefusalWithoutThrowing();

private:
    bool commitFile(const QString& name, const QString& content, const QString& message);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void MaintenanceTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("a\n"), QStringLiteral("a")));
    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("b\n"), QStringLiteral("b")));
    QVERIFY(git(m_path, {"repack", "-q", "-d"}));
    QVERIFY(commitFile(QStringLiteral("c.txt"), QStringLiteral("c\n"), QStringLiteral("c")));
    QVERIFY(commitFile(QStringLiteral("d.txt"), QStringLiteral("d\n"), QStringLiteral("d")));
    QVERIFY(git(m_path, {"repack", "-q", "-d"}));
}

void MaintenanceTest::cleanup() {
    m_dir.reset();
}

bool MaintenanceTest::commitFile(const QString& name, const QString& content,
                                 const QString& message) {
    if (!writeFile(m_path, name, content)) {
        return false;
    }
    if (!git(m_path, {"add", "-A"})) {
        return false;
    }
    QStringList args = kIdentity;
    args << "commit" << "-q" << "-m" << message;
    return git(m_path, args);
}

void MaintenanceTest::taskNamesRoundTrip() {
    for (const MaintenanceTask task :
         {MaintenanceTask::Gc, MaintenanceTask::Fsck, MaintenanceTask::CommitGraph,
          MaintenanceTask::MultiPackIndex, MaintenanceTask::Maintenance,
          MaintenanceTask::Optimize}) {
        MaintenanceTask parsed = MaintenanceTask::Gc;
        QVERIFY(maintenanceTaskFromName(maintenanceTaskName(task), &parsed));
        QVERIFY(parsed == task);
    }
    MaintenanceTask ignored = MaintenanceTask::Gc;
    QVERIFY(!maintenanceTaskFromName("frobnicate", &ignored));
}

void MaintenanceTest::commitGraphIsWrittenAndVerifies() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const SyncResult first = session.maintenanceBlocking(MaintenanceTask::CommitGraph);
    QVERIFY2(first.ok, first.output.c_str());
    QVERIFY(QFileInfo::exists(QDir(m_path).filePath(QStringLiteral(".git/objects/info/commit-graph"))));
    QVERIFY2(git(m_path, {"commit-graph", "verify"}), "git must verify the graph it wrote");

    const SyncResult second = session.maintenanceBlocking(MaintenanceTask::CommitGraph);
    QVERIFY2(second.ok, second.output.c_str());
}

void MaintenanceTest::multiPackIndexIsWrittenAndVerifies() {
    QVERIFY2(packCount(m_path) >= 2, "the fixture should leave more than one pack");

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const SyncResult result = session.maintenanceBlocking(MaintenanceTask::MultiPackIndex);
    QVERIFY2(result.ok, result.output.c_str());
    QVERIFY(QFileInfo::exists(
        QDir(m_path).filePath(QStringLiteral(".git/objects/pack/multi-pack-index"))));
    QVERIFY2(git(m_path, {"multi-pack-index", "verify"}), "git must verify the index it wrote");
}

void MaintenanceTest::optimizeWritesBothAndIsRepeatable() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const SyncResult first = session.maintenanceBlocking(MaintenanceTask::Optimize);
    QVERIFY2(first.ok, first.output.c_str());
    QVERIFY(QFileInfo::exists(QDir(m_path).filePath(QStringLiteral(".git/objects/info/commit-graph"))));
    QVERIFY(QFileInfo::exists(
        QDir(m_path).filePath(QStringLiteral(".git/objects/pack/multi-pack-index"))));

    const SyncResult second = session.maintenanceBlocking(MaintenanceTask::Optimize);
    QVERIFY2(second.ok, second.output.c_str());
}

void MaintenanceTest::maintenanceRunSucceeds() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const SyncResult result = session.maintenanceBlocking(MaintenanceTask::Maintenance);
    QVERIFY2(result.ok, result.output.c_str());
}

void MaintenanceTest::aTaskWithNothingToDoReportsGitsRefusalWithoutThrowing() {
    const QString empty = m_dir->filePath(QStringLiteral("loose-only"));
    QVERIFY(QDir().mkpath(empty));
    QVERIFY(git(empty, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(empty, QStringLiteral("a.txt"), QStringLiteral("a\n")));
    QVERIFY(git(empty, {"add", "-A"}));
    QStringList commit = kIdentity;
    commit << "commit" << "-q" << "-m" << "a";
    QVERIFY(git(empty, commit));
    QCOMPARE(packCount(empty), 0);

    RepoSession session;
    QString error;
    QVERIFY2(session.open(empty, &error), qPrintable(error));

    const SyncResult midx = session.maintenanceBlocking(MaintenanceTask::MultiPackIndex);
    QVERIFY(!midx.ok);
    QVERIFY(!midx.output.empty());

    const SyncResult graph = session.maintenanceBlocking(MaintenanceTask::CommitGraph);
    QVERIFY2(graph.ok, graph.output.c_str());
}

QTEST_MAIN(MaintenanceTest)
#include "test_maintenance.moc"
