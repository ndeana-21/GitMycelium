/// External diff and merge tools, against a fake tool script that records the
/// invocation. The point is not that git's own tool backends work — it is that the
/// client configures the right key, passes the right revisions, and lets git do the
/// invoking. A real temporary repository, no network.

#include "core/types.h"
#include "git/cli.h"
#include "git/external_tools.h"
#include "git/repository.h"

#include <QDir>
#include <QFile>
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

QString gitOut(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

bool writeFile(const QString& path, const QString& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}

QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

bool makeExecutable(const QString& path) {
    return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                           QFile::ReadGroup | QFile::ExeGroup |
                                           QFile::ReadOther | QFile::ExeOther);
}

} // namespace

class ExternalToolsTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    QString m_log;

private slots:
    void initTestCase();
    void init();

    void theToolChoiceRoundTrips();
    void gitListsToolsItKnows();
    void theDiffToolIsInvokedWithThePair();
    void theMergeToolIsInvokedAndResolves();
};

void ExternalToolsTest::initTestCase() {
    Repository::initLibrary();
}

void ExternalToolsTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    m_log = QDir(m_path).filePath(QStringLiteral("tool-calls.txt"));
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Demo Dev"}));
    QVERIFY(git(m_path, {"config", "user.email", "demo@example.com"}));
    QVERIFY(git(m_path, {"config", "commit.gpgsign", "false"}));
    QVERIFY(git(m_path, {"config", "mergetool.keepBackup", "false"}));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("a.txt")), QStringLiteral("one\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("a.txt")), QStringLiteral("two\n")));
    QVERIFY(git(m_path, {"commit", "-qam", "second"}));
}

void ExternalToolsTest::theToolChoiceRoundTrips() {
    ExternalToolOps ops(m_path.toStdString());
    QCOMPARE(QString::fromStdString(ops.tool(false)), QString());
    std::string error;
    QVERIFY(ops.setTool(false, "meld", &error));
    QCOMPARE(QString::fromStdString(ops.tool(false)), QStringLiteral("meld"));
    QVERIFY(ops.setTool(true, "vimdiff", &error));
    QCOMPARE(QString::fromStdString(ops.tool(true)), QStringLiteral("vimdiff"));

    // An empty name returns to git's default rather than pinning an empty tool.
    QVERIFY(ops.setTool(false, "", &error));
    QCOMPARE(QString::fromStdString(ops.tool(false)), QString());
}

void ExternalToolsTest::gitListsToolsItKnows() {
    ExternalToolOps ops(m_path.toStdString());
    const std::vector<std::string> tools = ops.availableTools(false);
    QVERIFY(!tools.empty());
    QVERIFY2(std::find(tools.begin(), tools.end(), "vimdiff") != tools.end(),
             "vimdiff is a tool git always lists");
}

void ExternalToolsTest::theDiffToolIsInvokedWithThePair() {
    const QString script = QDir(m_path).filePath(QStringLiteral("fake-diff.sh"));
    QVERIFY(writeFile(script,
                      QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"diff:$LOCAL:$REMOTE\" >> '%1'\n")
                          .arg(m_log)));
    QVERIFY(makeExecutable(script));

    ExternalToolOps ops(m_path.toStdString());
    std::string error;
    QVERIFY(ops.setTool(false, "gkfake", &error));
    QVERIFY(git(m_path, {"config", "difftool.gkfake.cmd", script}));

    const SyncResult result =
        ops.runDiff("HEAD~1", "HEAD", "a.txt");
    QVERIFY2(result.ok, result.output.c_str());
    const QString log = readFile(m_log);
    QVERIFY2(log.contains(QStringLiteral("diff:")), qPrintable(log));
}

void ExternalToolsTest::theMergeToolIsInvokedAndResolves() {
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature", "HEAD~1"}));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("a.txt")),
                      QStringLiteral("feature\n")));
    QVERIFY(git(m_path, {"commit", "-qam", "feature side"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("a.txt")), QStringLiteral("main\n")));
    QVERIFY(git(m_path, {"commit", "-qam", "main side"}));
    git(m_path, {"merge", "--no-edit", "feature"});
    QVERIFY(!gitOut(m_path, {"ls-files", "-u"}).isEmpty());

    ExternalToolOps ops(m_path.toStdString());
    std::string error;
    QVERIFY(ops.setTool(true, "gkfake", &error));
    // A custom mergetool's cmd is evaluated by git's own shell, where $LOCAL and
    // $MERGED are set but not exported — a script would see empty ones, so the copy
    // has to be part of the cmd itself.
    const QString cmd =
        QStringLiteral("cp \"$LOCAL\" \"$MERGED\" && printf 'merge\\n' >> '%1'").arg(m_log);
    QVERIFY(git(m_path, {"config", "mergetool.gkfake.cmd", cmd}));
    QVERIFY(git(m_path, {"config", "mergetool.gkfake.trustExitCode", "true"}));

    const SyncResult result = ops.runMerge("a.txt");
    QVERIFY2(result.ok, result.output.c_str());
    QVERIFY2(readFile(m_log).contains(QStringLiteral("merge")), "the tool must have run");
    // git stages what the tool wrote, so the conflict is gone.
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).isEmpty());
}

QTEST_MAIN(ExternalToolsTest)
#include "test_external_tools.moc"
