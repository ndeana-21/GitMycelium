/// The Files tab: a commit's tree, a file in it, the file's history across a rename,
/// the change a commit made, and the history of selected lines (`git log -L`).

#include "git/cli.h"
#include "ui/file_browser.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
#include <QTabBar>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).exitCode == 0;
}

QString gitOut(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

bool writeFile(const QString& path, const QString& content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content.toUtf8()) == content.toUtf8().size();
}

} // namespace

class FileBrowserTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void aFileIsReadAndTracedAcrossARename();

private:
    bool commit(const QString& message);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

bool FileBrowserTest::commit(const QString& message) {
    return git(m_path, {"add", "-A"}) && git(m_path, {"commit", "-qm", message});
}

void FileBrowserTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "T"}));
    QVERIFY(git(m_path, {"config", "user.email", "t@x"}));
    QVERIFY(writeFile(m_path + "/a.txt", "one\ntwo\nthree\nfour\nfive\n"));
    QVERIFY(writeFile(m_path + "/readme.md", "readme\n"));
    QVERIFY(commit("create a"));
    QVERIFY(writeFile(m_path + "/a.txt", "one\ntwo changed\nthree\nfour\nfive\n"));
    QVERIFY(commit("change line two"));
    QVERIFY(git(m_path, {"mv", "a.txt", "dir/b.txt"}) ||
            (QDir(m_path).mkpath("dir") && git(m_path, {"mv", "a.txt", "dir/b.txt"})));
    QVERIFY(commit("move a to dir/b"));
    QVERIFY(writeFile(m_path + "/dir/b.txt", "one\ntwo changed\nthree\nfour\nfive changed\n"));
    QVERIFY(commit("change line five"));
}

void FileBrowserTest::cleanup() {
    m_dir.reset();
}

void FileBrowserTest::aFileIsReadAndTracedAcrossARename() {
    MainWindow window;
    window.setHeadless(true);
    QVERIFY(window.openRepository(m_path));
    window.showFilesPageForDiagnostics();
    auto* tabs = window.findChild<QTabBar*>(QStringLiteral("centerTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->currentIndex(), 1);
    FileBrowser* files = window.fileBrowserForDiagnostics();
    QVERIFY(files);
    QTRY_VERIFY(!files->busy() && !files->treeRowsForDiagnostics().isEmpty());
    QCOMPARE(files->treeRowsForDiagnostics(),
             (QStringList{QStringLiteral("dir/"), QStringLiteral("  …"), QStringLiteral("readme.md")}));
    files->expandForDiagnostics(QStringLiteral("dir"));
    QTRY_VERIFY(!files->busy());
    QVERIFY(files->treeRowsForDiagnostics().contains(QStringLiteral("  b.txt")));

    files->openFile(QStringLiteral("dir/b.txt"));
    QTRY_VERIFY(!files->busy());
    QCOMPARE(files->viewerText(), QStringLiteral("one\ntwo changed\nthree\nfour\nfive changed\n"));
    QStringList history = files->historyRowsForDiagnostics();
    QVERIFY2(history.size() == 5, qPrintable(history.join('\n')));
    QVERIFY(history.at(4).endsWith(QStringLiteral("create a")));

    // The change before the rename is shown under the name the file had then.
    files->showHistoryEntry(2);
    QTRY_VERIFY(!files->busy());
    QVERIFY2(files->viewerText().contains(QStringLiteral("+two changed")),
             qPrintable(files->viewerText()));
    QVERIFY(files->viewerTitle().startsWith(QStringLiteral("a.txt in ")));

    // Line two was written by the first commit and changed by the second, nothing else.
    files->showLineHistory(2, 2);
    QTRY_VERIFY(!files->busy() && files->historyRowsForDiagnostics().size() == 3);
    history = files->historyRowsForDiagnostics();
    QVERIFY2(history.at(1).endsWith(QStringLiteral("change line two")), qPrintable(history.join('\n')));
    QVERIFY(history.at(2).endsWith(QStringLiteral("create a")));
    files->showHistoryEntry(0);
    QVERIFY(files->viewerText().contains(QStringLiteral("+two changed")));
    QVERIFY(!files->viewerText().contains(QStringLiteral("five")));

    // An older commit shows its own tree.
    files->setRevision(gitOut(m_path, {"rev-parse", "HEAD~2"}), QStringLiteral("old"));
    QTRY_VERIFY(!files->busy() &&
                files->treeRowsForDiagnostics().contains(QStringLiteral("a.txt")));
}

QTEST_MAIN(FileBrowserTest)
#include "test_file_browser.moc"
