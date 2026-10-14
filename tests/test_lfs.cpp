/// `git-lfs` is optional, so every test here starts with a check and skips rather
/// than failing on a machine that does not have it. The parsers are exercised on
/// their own, without the binary, so that part is covered everywhere.

#include "git/cli.h"
#include "git/lfs.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

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

QString gitOut(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

bool writeFile(const QString& path, const QByteArray& content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return f.write(content) == content.size();
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/// A deterministic 4 KiB body with a NUL in it — the shape a text comparison would
/// mangle, so the round trip has to be byte-for-byte.
QByteArray binaryBody() {
    QByteArray body(4096, '\0');
    for (int i = 0; i < body.size(); ++i) {
        body[i] = static_cast<char>((i * 31) & 0xFF);
    }
    return body;
}

} // namespace

class LfsTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void parseLsFilesDistinguishesPresentFromMissing();
    void parseTrackedReadsBothSections();

    void availabilityAndVersion();
    void installWritesTheLocalFilterConfig();
    void trackAndUntrackEditGitattributes();
    void lfsFilesListCommittedFiles();
    void pruneRuns();
    void pushAndPullRoundTripThroughALocalRemote();
    void absentGitLfsIsReportedNotThrown();
    void stagingAnLfsFileStoresItsPointer();
    void checkingOutABranchMaterialisesLfsContent();

private:
    /// True when git-lfs is on this machine. The QSKIP itself lives in each test
    /// slot: it expands to a bare `return`, which a helper returning bool cannot do.
    bool lfsPresent() const { return GitLfsOps(m_path.toStdString()).available(); }

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void LfsTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(m_path + "/a.txt", "hello\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
}

void LfsTest::cleanup() {
    m_dir.reset();
}

void LfsTest::parseLsFilesDistinguishesPresentFromMissing() {
    const std::string output =
        "b2197cacfc * data.bin\n"
        "deadbeef00 - my file with spaces.psd\n"
        "\n";
    const std::vector<LfsFile> files = parseLfsLsFiles(output);
    QCOMPARE(files.size(), static_cast<std::size_t>(2));
    QCOMPARE(QString::fromStdString(files.at(0).oid), QStringLiteral("b2197cacfc"));
    QVERIFY(files.at(0).objectPresent);
    QCOMPARE(QString::fromStdString(files.at(0).path), QStringLiteral("data.bin"));
    QVERIFY(!files.at(1).objectPresent);
    QCOMPARE(QString::fromStdString(files.at(1).path), QStringLiteral("my file with spaces.psd"));
}

void LfsTest::parseTrackedReadsBothSections() {
    const std::string output =
        "Listing tracked patterns\n"
        "    *.bin (.gitattributes)\n"
        "    art/*.psd (.gitattributes)\n"
        "Listing excluded patterns\n"
        "    *.tmp\n";
    const std::vector<LfsPattern> patterns = parseLfsTracked(output);
    QCOMPARE(patterns.size(), static_cast<std::size_t>(2));
    QCOMPARE(QString::fromStdString(patterns.at(0).pattern), QStringLiteral("*.bin"));
    QCOMPARE(QString::fromStdString(patterns.at(0).source), QStringLiteral(".gitattributes"));
    QCOMPARE(QString::fromStdString(patterns.at(1).pattern), QStringLiteral("art/*.psd"));
}

void LfsTest::availabilityAndVersion() {
    if (!lfsPresent()) {
        QSKIP("git-lfs is not installed");
    }
    RepoSession session;
    QVERIFY(session.open(m_path));
    QVERIFY(session.lfsAvailable());
    QVERIFY(session.lfsVersion().contains(QStringLiteral("git-lfs")));
}

void LfsTest::installWritesTheLocalFilterConfig() {
    if (!lfsPresent()) {
        QSKIP("git-lfs is not installed");
    }
    RepoSession session;
    QVERIFY(session.open(m_path));
    QVERIFY(!session.lfsConfigured());

    QString error;
    QVERIFY2(session.lfsInstall(&error), qPrintable(error));
    QVERIFY(session.lfsConfigured());
    // Local, not global: the repository is where the filter belongs.
    QVERIFY(!gitOut(m_path, {"config", "--local", "--get", "filter.lfs.clean"}).isEmpty());
}

void LfsTest::trackAndUntrackEditGitattributes() {
    if (!lfsPresent()) {
        QSKIP("git-lfs is not installed");
    }
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.lfsInstall(&error), qPrintable(error));

    QVERIFY2(session.lfsTrack(QStringLiteral("*.bin"), &error), qPrintable(error));
    QVERIFY(QFile::exists(m_path + "/.gitattributes"));
    QVERIFY(readFile(m_path + "/.gitattributes").contains("filter=lfs"));
    QVERIFY(session.lfsTrackedPatterns().join(QLatin1Char(';')).contains(QStringLiteral("*.bin")));
    QVERIFY(!gitOut(m_path, {"diff", "--cached", "--name-only"}).contains(QStringLiteral(".gitattributes")));

    QVERIFY2(session.lfsUntrack(QStringLiteral("*.bin"), &error), qPrintable(error));
    QVERIFY(!readFile(m_path + "/.gitattributes").contains("filter=lfs"));
}

void LfsTest::lfsFilesListCommittedFiles() {
    if (!lfsPresent()) {
        QSKIP("git-lfs is not installed");
    }
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.lfsInstall(&error), qPrintable(error));
    QVERIFY2(session.lfsTrack(QStringLiteral("*.bin"), &error), qPrintable(error));
    QVERIFY(writeFile(m_path + "/blob.bin", binaryBody()));
    QVERIFY(git(m_path, {"add", ".gitattributes", "blob.bin"}));
    QVERIFY(git(m_path, {"commit", "-qm", "add blob"}));

    const QStringList files = session.lfsFiles();
    QCOMPARE(files.size(), 1);
    QVERIFY2(files.first().startsWith(QStringLiteral("* ")), qPrintable(files.join(QLatin1Char(';'))));
    QVERIFY(files.first().endsWith(QStringLiteral(" blob.bin")));
    QVERIFY(gitOut(m_path, {"show", "HEAD:blob.bin"}).startsWith(QStringLiteral("version https://git-lfs")));
}

void LfsTest::pruneRuns() {
    if (!lfsPresent()) {
        QSKIP("git-lfs is not installed");
    }
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.lfsInstall(&error), qPrintable(error));
    QVERIFY2(session.lfsTrack(QStringLiteral("*.bin"), &error), qPrintable(error));
    QVERIFY(writeFile(m_path + "/blob.bin", binaryBody()));
    QVERIFY(git(m_path, {"add", ".gitattributes", "blob.bin"}));
    QVERIFY(git(m_path, {"commit", "-qm", "add blob"}));

    const SyncResult result = session.lfsBlocking(LfsTask::Prune, {}, {});
    QVERIFY2(result.ok, result.output.c_str());
}

void LfsTest::pushAndPullRoundTripThroughALocalRemote() {
    if (!lfsPresent()) {
        QSKIP("git-lfs is not installed");
    }
    QTemporaryDir sandbox;
    QVERIFY(sandbox.isValid());
    const QString bare = sandbox.path() + "/origin.git";
    const QString clone = sandbox.path() + "/clone";
    QVERIFY(git(sandbox.path(), {"init", "-q", "--bare", "origin.git"}));
    QVERIFY(git(bare, {"symbolic-ref", "HEAD", "refs/heads/main"}));

    RepoSession source;
    QVERIFY(source.open(m_path));
    QString error;
    QVERIFY2(source.lfsInstall(&error), qPrintable(error));
    QVERIFY2(source.lfsTrack(QStringLiteral("*.bin"), &error), qPrintable(error));
    const QByteArray body = binaryBody();
    QVERIFY(writeFile(m_path + "/blob.bin", body));
    QVERIFY(git(m_path, {"add", ".gitattributes", "blob.bin"}));
    QVERIFY(git(m_path, {"commit", "-qm", "add blob"}));
    QVERIFY(git(m_path, {"remote", "add", "origin", bare}));
    QVERIFY(git(m_path, {"push", "-q", "origin", "main"}));
    const SyncResult pushed = source.lfsBlocking(LfsTask::PushAll, QStringLiteral("origin"), {});
    QVERIFY2(pushed.ok, pushed.output.c_str());

    QVERIFY(git(sandbox.path(), {"clone", "-q", "-b", "main", bare, clone}));
    // The clone has no filter config yet, so the pointer is on disk…
    QCOMPARE(readFile(clone + "/blob.bin").size(), 129);
    QVERIFY(readFile(clone + "/blob.bin").startsWith("version https://git-lfs"));

    RepoSession copy;
    QVERIFY(copy.open(clone));
    QVERIFY2(copy.lfsInstall(&error), qPrintable(error));
    const SyncResult pulled = copy.lfsBlocking(LfsTask::Pull, QStringLiteral("origin"), {});
    QVERIFY2(pulled.ok, pulled.output.c_str());
    QCOMPARE(readFile(clone + "/blob.bin"), body);
}

void LfsTest::absentGitLfsIsReportedNotThrown() {
    // Hide git-lfs behind a PATH that still has git. Most machines find git-lfs on
    // PATH, but one installed next to git itself stays reachable through git's exec
    // path, so the test steps aside rather than failing on that.
    const QString gitPath = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (gitPath.isEmpty()) {
        QSKIP("git is not on PATH");
    }
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    QVERIFY(QFile::link(gitPath, bin.path() + "/git"));
    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", bin.path().toUtf8());
    const bool hidden = !GitLfsOps(m_path.toStdString()).available();
    const std::string version = GitLfsOps(m_path.toStdString()).version();
    const SyncResult install = GitLfsOps(m_path.toStdString()).install();
    qputenv("PATH", oldPath);

    if (!hidden) {
        QSKIP("git-lfs is still reachable through git's exec path on this machine");
    }
    QVERIFY(version.empty());
    QVERIFY(!install.ok);
    QVERIFY(!install.output.empty());
}

void LfsTest::stagingAnLfsFileStoresItsPointer() {
    if (!lfsPresent()) {
        QSKIP("git-lfs is not installed");
    }
    QVERIFY(git(m_path, {"lfs", "install", "--local"}));
    QVERIFY(git(m_path, {"lfs", "track", "*.bin"}));
    QVERIFY(writeFile(m_path + "/data.bin", binaryBody()));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.stageFile(QStringLiteral("data.bin"), &error), qPrintable(error));
    QVERIFY(gitOut(m_path, {"cat-file", "-p", ":data.bin"})
                .startsWith(QStringLiteral("version https://git-lfs.github.com/spec/v1")));

    QVERIFY(writeFile(m_path + "/more.bin", binaryBody() + "more"));
    QVERIFY2(session.stageAll(&error), qPrintable(error));
    QVERIFY(gitOut(m_path, {"cat-file", "-p", ":more.bin"})
                .startsWith(QStringLiteral("version https://git-lfs.github.com/spec/v1")));
}

void LfsTest::checkingOutABranchMaterialisesLfsContent() {
    if (!lfsPresent()) {
        QSKIP("git-lfs is not installed");
    }
    QVERIFY(git(m_path, {"lfs", "install", "--local"}));
    QVERIFY(git(m_path, {"lfs", "track", "*.bin"}));
    QVERIFY(git(m_path, {"add", ".gitattributes"}));
    QVERIFY(git(m_path, {"commit", "-qm", "track"}));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "assets"}));
    QVERIFY(writeFile(m_path + "/data.bin", binaryBody()));
    QVERIFY(git(m_path, {"add", "data.bin"}));
    QVERIFY(git(m_path, {"commit", "-qm", "asset"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.checkoutBranch(QStringLiteral("assets"), &error), qPrintable(error));
    QCOMPARE(readFile(m_path + "/data.bin"), binaryBody());
}

QTEST_MAIN(LfsTest)
#include "test_lfs.moc"
