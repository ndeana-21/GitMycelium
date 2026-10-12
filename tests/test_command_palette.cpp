/// The command palette: a command is data, so it can be listed, filtered and run
/// without a menu. The registry's matching is the part worth pinning down — a
/// palette whose order changes between runs is a palette nobody can use from the
/// keyboard — and the window's headless report proves a command really runs.

#include "git/cli.h"
#include "model/commit_model.h"
#include "ui/command_palette.h"
#include "ui/commit_graph_view.h"
#include "ui/main_window.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace gk;

namespace {

bool git(const QString& workDir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const auto& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, workDir.toStdString()).exitCode == 0;
}

bool writeFile(const QString& path, const QByteArray& content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    return f.write(content) == content.size();
}

QStringList idsOf(const std::vector<const Command*>& commands) {
    QStringList ids;
    for (const Command* command : commands) {
        ids << command->id;
    }
    return ids;
}

} // namespace

class CommandPaletteTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

private slots:
    void init();
    void cleanup();

    void matchingIsCaseInsensitiveAndRanked();
    void everyQueryWordMustMatch();
    void aDisabledCommandIsShownButNotChosen();
    void thePaletteChoosesWithoutRunning();
    void theWindowExposesAndRunsACommand();
    void aCommandThatRefreshesTheWindowOutlivesTheRebuild();
};

void CommandPaletteTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    git(m_path, {"init", "-q", "-b", "main"});
    git(m_path, {"config", "user.name", "Demo"});
    git(m_path, {"config", "user.email", "demo@example.com"});
    git(m_path, {"config", "commit.gpgsign", "false"});
    QVERIFY(writeFile(m_path + "/app.txt", "line 1\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "first"}));
}

void CommandPaletteTest::cleanup() {
    m_dir.reset();
}

void CommandPaletteTest::matchingIsCaseInsensitiveAndRanked() {
    CommandRegistry registry;
    registry.add({QStringLiteral("branch.new"), QStringLiteral("New Branch…"),
                  QStringLiteral("Branch"), {}, {}, {}});
    registry.add({QStringLiteral("branch.merge"),
                  QStringLiteral("Merge Branch Into Current…"), QStringLiteral("Branch"), {}, {}, {}});
    registry.add({QStringLiteral("remote.fetch"), QStringLiteral("Fetch"),
                  QStringLiteral("Remote"), {}, {}, {}});
    registry.add({QStringLiteral("view.branches"), QStringLiteral("Toggle Branches Panel"),
                  QStringLiteral("View"), {}, {}, {}});

    QCOMPARE(idsOf(registry.search(QStringLiteral("branch"))),
             QStringList({QStringLiteral("branch.merge"), QStringLiteral("branch.new"),
                          QStringLiteral("view.branches")}));
    QCOMPARE(idsOf(registry.search(QStringLiteral("BRANCH"))),
             idsOf(registry.search(QStringLiteral("branch"))));
    QCOMPARE(idsOf(registry.search(QStringLiteral("fetch"))),
             QStringList({QStringLiteral("remote.fetch")}));
    QCOMPARE(registry.search(QString()).size(), std::size_t(4));
}

void CommandPaletteTest::everyQueryWordMustMatch() {
    CommandRegistry registry;
    registry.add({QStringLiteral("view.branches"), QStringLiteral("Toggle Branches Panel"),
                  QStringLiteral("View"), {}, {}, {}});
    registry.add({QStringLiteral("view.conflicts"), QStringLiteral("Show Conflicts"),
                  QStringLiteral("View"), {}, {}, {}});
    registry.add({QStringLiteral("branch.new"), QStringLiteral("New Branch…"),
                  QStringLiteral("Branch"), {}, {}, {}});

    // Both words must land: "toggle" is in one title, "conflicts" in another, and
    // a palette that matched either would answer a question nobody asked.
    QCOMPARE(idsOf(registry.search(QStringLiteral("toggle panel"))),
             QStringList({QStringLiteral("view.branches")}));
    QVERIFY(registry.search(QStringLiteral("toggle conflicts")).empty());
}

void CommandPaletteTest::aDisabledCommandIsShownButNotChosen() {
    bool ran = false;
    CommandRegistry registry;
    registry.add({QStringLiteral("x.off"), QStringLiteral("Disabled Thing"), QStringLiteral("X"),
                  {}, [] { return false; }, [&ran] { ran = true; }});

    CommandPalette palette(registry);
    QCOMPARE(palette.matchesFor(QStringLiteral("disabled")),
             QStringList({QStringLiteral("x.off")}));
    QCOMPARE(palette.chooseFirst(QStringLiteral("disabled")), QString());
    QVERIFY(!ran);
}

void CommandPaletteTest::thePaletteChoosesWithoutRunning() {
    bool ran = false;
    CommandRegistry registry;
    registry.add({QStringLiteral("x.on"), QStringLiteral("Enabled Thing"), QStringLiteral("X"),
                  {}, {}, [&ran] { ran = true; }});

    CommandPalette palette(registry);
    QCOMPARE(palette.chooseFirst(QStringLiteral("enabled")), QStringLiteral("x.on"));
    // Choosing is not running: the window runs the command after the palette closes,
    // so a command with its own dialog does not open it under a modal palette.
    QVERIFY(!ran);
}

void CommandPaletteTest::theWindowExposesAndRunsACommand() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QStringList matches = window.commandPaletteForDiagnostics(QStringLiteral("merge"), false);
    QVERIFY2(matches.join(QLatin1Char('\n')).contains(QStringLiteral("branch.merge")),
             qPrintable(matches.join(QLatin1Char('\n'))));

    auto* view = window.findChild<CommitGraphView*>();
    QVERIFY(view);
    QVERIFY(view->isColumnHidden(CommitModel::Column::Sha));
    const QStringList run =
        window.commandPaletteForDiagnostics(QStringLiteral("toggle sha"), true);
    QVERIFY2(run.join(QLatin1Char('\n')).contains(QStringLiteral("chosen: view.column.sha")),
             qPrintable(run.join(QLatin1Char('\n'))));
    QVERIFY(!view->isColumnHidden(CommitModel::Column::Sha));
}

void CommandPaletteTest::aCommandThatRefreshesTheWindowOutlivesTheRebuild() {
    // A checkout refreshes the window, and the refresh rebuilds the registry the
    // running command lives in. Its captured branch name used to be freed under it
    // (a bad_alloc on the next use); run under ASan this reads freed memory.
    const QString branch = QStringLiteral("feature/a-branch-name-long-enough-to-be-on-the-heap");
    QVERIFY(git(m_path, {"branch", branch}));

    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    window.commandPaletteForDiagnostics(QStringLiteral("checkout a-branch-name"), true);

    QCOMPARE(QString::fromStdString(runGit({"branch", "--show-current"}, m_path.toStdString()).out)
                 .trimmed(),
             branch);
}

QTEST_MAIN(CommandPaletteTest)
#include "test_command_palette.moc"
