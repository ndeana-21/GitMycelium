#include "git/cli.h"
#include "git/hooks.h"
#include "repo/repo_session.h"
#include "ui/hooks_dialog.h"
#include "ui/main_window.h"

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

/// Whether git would run the file as a hook: its executable bit on POSIX; git for
/// Windows runs any hook file that exists.
bool runsAsHook(const QString& path) {
#ifdef Q_OS_WIN
    return QFileInfo::exists(path);
#else
    return QFileInfo(path).isExecutable();
#endif
}

const HookEntry* findHook(const std::vector<HookEntry>& hooks, const QString& name) {
    for (const HookEntry& hook : hooks) {
        if (QString::fromStdString(hook.name) == name) {
            return &hook;
        }
    }
    return nullptr;
}

} // namespace

class HooksTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theListReportsSamplesAndServerHooks();
    void enablingAdoptsTheSample();
    void writingDisablingAndEnabling();
    void hooksPathFollowsTheConfig();
    void enablingSomethingThatDoesNotExistFails();
    void theWindowListsAndTogglesHooks();
    void theDialogReportsTheReadersChoice();
    void aFailingPreCommitHookStopsTheCommit();
    void aCommitMsgHookShapesTheCommittedMessage();
    void finishingAMergeRunsTheHooksToo();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void HooksTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("a\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "a"}));
}

void HooksTest::cleanup() {
    m_dir.reset();
}

void HooksTest::theListReportsSamplesAndServerHooks() {
    const std::vector<HookEntry> hooks = HooksOps(m_path.toStdString()).list();

    const HookEntry* commit = findHook(hooks, QStringLiteral("pre-commit"));
    QVERIFY(commit != nullptr);
    QVERIFY(commit->state == HookState::Sample);
    QVERIFY(!commit->serverSide);
    QVERIFY(QFileInfo(QString::fromStdString(commit->path))
                .fileName()
                .startsWith(QStringLiteral("pre-commit")));

    const HookEntry* receive = findHook(hooks, QStringLiteral("pre-receive"));
    QVERIFY(receive != nullptr);
    QVERIFY(receive->serverSide);
}

void HooksTest::enablingAdoptsTheSample() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    QVERIFY2(session.enableHook(QStringLiteral("pre-commit"), &error), qPrintable(error));

    // Bind the vector: a pointer into `session.hooks()`'s temporary dangles.
    const std::vector<HookEntry> hooks = session.hooks();
    const HookEntry* hook = findHook(hooks, QStringLiteral("pre-commit"));
    QVERIFY(hook != nullptr);
    QVERIFY(hook->state == HookState::Enabled);
    QVERIFY(runsAsHook(QString::fromStdString(hook->path)));
    QVERIFY(!session.hookContent(QStringLiteral("pre-commit")).isEmpty());
    QVERIFY(QFileInfo::exists(
        QDir(m_path).filePath(QStringLiteral(".git/hooks/pre-commit.sample"))));
}

void HooksTest::writingDisablingAndEnabling() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    QVERIFY2(session.writeHook(QStringLiteral("pre-commit"), QStringLiteral("#!/bin/sh\nexit 0\n"),
                               &error),
             qPrintable(error));

    const HookEntry* written = findHook(session.hooks(), QStringLiteral("pre-commit"));
    QVERIFY(written != nullptr);
    QVERIFY(written->state == HookState::Disabled);
    QCOMPARE(session.hookContent(QStringLiteral("pre-commit")).trimmed(),
             QStringLiteral("#!/bin/sh\nexit 0"));

    QVERIFY2(session.enableHook(QStringLiteral("pre-commit"), &error), qPrintable(error));
    QVERIFY(findHook(session.hooks(), QStringLiteral("pre-commit"))->state == HookState::Enabled);
    QVERIFY(runsAsHook(
        QString::fromStdString(findHook(session.hooks(), QStringLiteral("pre-commit"))->path)));

    QVERIFY2(session.disableHook(QStringLiteral("pre-commit"), &error), qPrintable(error));
    QVERIFY(findHook(session.hooks(), QStringLiteral("pre-commit"))->state == HookState::Disabled);
    QCOMPARE(session.hookContent(QStringLiteral("pre-commit")).trimmed(),
             QStringLiteral("#!/bin/sh\nexit 0"));
}

void HooksTest::hooksPathFollowsTheConfig() {
    QVERIFY(git(m_path, {"config", "core.hooksPath", "myhooks"}));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const QString path = session.hooksPath();
    QVERIFY2(path.endsWith(QStringLiteral("myhooks")), qPrintable(path));
    QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("myhooks"));

    QVERIFY2(session.writeHook(QStringLiteral("pre-commit"), QStringLiteral("#!/bin/sh\n"),
                               &error),
             qPrintable(error));
    QVERIFY2(session.enableHook(QStringLiteral("pre-commit"), &error), qPrintable(error));
    QVERIFY(runsAsHook(QDir(path).filePath(QStringLiteral("pre-commit"))));
    QVERIFY(!QFileInfo::exists(QDir(m_path).filePath(QStringLiteral(".git/hooks/pre-commit"))));
}

void HooksTest::enablingSomethingThatDoesNotExistFails() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    QString failure;
    QVERIFY(!session.enableHook(QStringLiteral("not-a-hook"), &failure));
    QVERIFY(!failure.isEmpty());
}

void HooksTest::theWindowListsAndTogglesHooks() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QString before = window.hooksForDiagnostics(QStringLiteral("list"), {}).join(QLatin1Char('\n'));
    QVERIFY2(before.contains(QStringLiteral("pre-commit: sample")), qPrintable(before));
    QVERIFY(before.contains(QStringLiteral("pre-receive:")));

    const QString enabled =
        window.hooksForDiagnostics(QStringLiteral("enable"), {QStringLiteral("pre-commit")})
            .join(QLatin1Char('\n'));
    QVERIFY2(enabled.contains(QStringLiteral("enable pre-commit: ok")), qPrintable(enabled));

    const QString after = window.hooksForDiagnostics(QStringLiteral("list"), {}).join(QLatin1Char('\n'));
    QVERIFY2(after.contains(QStringLiteral("pre-commit: enabled")), qPrintable(after));

    const QStringList shown =
        window.hooksForDiagnostics(QStringLiteral("show"), {QStringLiteral("pre-commit")});
    QVERIFY(!shown.isEmpty());
    QVERIFY(!shown.first().contains(QStringLiteral("(empty)")));

    const QString disabled =
        window.hooksForDiagnostics(QStringLiteral("disable"), {QStringLiteral("pre-commit")})
            .join(QLatin1Char('\n'));
    QVERIFY2(disabled.contains(QStringLiteral("disable pre-commit: ok")), qPrintable(disabled));
}

void HooksTest::theDialogReportsTheReadersChoice() {
    std::vector<HookEntry> hooks;
    HookEntry commit;
    commit.name = "pre-commit";
    commit.path = "/tmp/x/pre-commit";
    commit.state = HookState::Sample;
    hooks.push_back(commit);
    HookEntry receive;
    receive.name = "pre-receive";
    receive.state = HookState::Disabled;
    receive.serverSide = true;
    hooks.push_back(receive);

    HooksDialog dialog(hooks, {{QStringLiteral("pre-commit"), QStringLiteral("#!/bin/sh\n")}},
                       QStringLiteral("/tmp/x"), nullptr);

    QVERIFY(!dialog.hookEnabled(QStringLiteral("pre-commit")));
    QVERIFY(dialog.enabledNames().isEmpty());
    dialog.setHookEnabled(QStringLiteral("pre-commit"), true);
    QVERIFY(dialog.hookEnabled(QStringLiteral("pre-commit")));
    QCOMPARE(dialog.enabledNames(), QStringList{QStringLiteral("pre-commit")});

    // A server-side hook has no switch: it runs on the remote, not here.
    dialog.setHookEnabled(QStringLiteral("pre-receive"), true);
    QVERIFY(!dialog.hookEnabled(QStringLiteral("pre-receive")));
    QVERIFY(!dialog.enabledNames().contains(QStringLiteral("pre-receive")));
}

namespace {

bool installHook(const QString& repo, const QString& name, const QString& script) {
    const QString path = QDir(repo).filePath(QStringLiteral(".git/hooks/") + name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(script.toUtf8());
    file.close();
    return file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
}

QString gitOut(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

} // namespace

void HooksTest::aFailingPreCommitHookStopsTheCommit() {
    QVERIFY(installHook(m_path, QStringLiteral("pre-commit"),
                        QStringLiteral("#!/bin/sh\necho lint failed >&2\nexit 1\n")));
    const QString before = gitOut(m_path, {"rev-parse", "HEAD"});

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("changed\n")));
    QVERIFY2(session.stageAll(&error), qPrintable(error));

    QVERIFY(!session.createCommit(QStringLiteral("blocked"), nullptr, &error));
    QVERIFY2(error.contains(QStringLiteral("lint failed")), qPrintable(error));
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD"}), before);
}

void HooksTest::aCommitMsgHookShapesTheCommittedMessage() {
    QVERIFY(installHook(m_path, QStringLiteral("commit-msg"),
                        QStringLiteral("#!/bin/sh\nprintf '\\nTicket: GM-1\\n' >> \"$1\"\n")));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("changed\n")));
    QVERIFY2(session.stageAll(&error), qPrintable(error));
    QVERIFY2(session.createCommit(QStringLiteral("subject"), nullptr, &error), qPrintable(error));

    QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%B"}),
             QStringLiteral("subject\nTicket: GM-1"));
}

void HooksTest::finishingAMergeRunsTheHooksToo() {
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "side"}));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("side\n")));
    QVERIFY(git(m_path, {"commit", "-qam", "side"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("main\n")));
    QVERIFY(git(m_path, {"commit", "-qam", "main"}));
    git(m_path, {"merge", "side"}); // conflicts, on purpose
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("both\n")));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(installHook(m_path, QStringLiteral("commit-msg"),
                        QStringLiteral("#!/bin/sh\nprintf '\\nhooked\\n' >> \"$1\"\n")));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));
    QVERIFY2(session.commitMerge(QStringLiteral("Merge side"), &error), qPrintable(error));

    QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%B"}), QStringLiteral("Merge side\nhooked"));
    QCOMPARE(gitOut(m_path, {"rev-list", "--parents", "-1", "HEAD"}).split(QLatin1Char(' ')).size(),
             3);
    QVERIFY(!QFile::exists(QDir(m_path).filePath(QStringLiteral(".git/MERGE_HEAD"))));
}

QTEST_MAIN(HooksTest)
#include "test_hooks.moc"
