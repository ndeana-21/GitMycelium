/// The Files tab: the working tree (editable) or a commit's tree (read-only), a file in
/// it, the file's history across a rename, the change a commit made (in the diff view),
/// and the history of selected lines (`git log -L`).

#include "git/cli.h"
#include "ui/diff_view.h"
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

bool writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content) == content.size();
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// Every added line of a diff, `+` and all.
QStringList addedLines(const FileDiff& diff) {
    QStringList out;
    for (const DiffHunk& hunk : diff.hunks) {
        for (const DiffLine& line : hunk.lines) {
            if (line.kind == DiffLineKind::Added) {
                out << QStringLiteral("+") + QString::fromStdString(line.text);
            }
        }
    }
    return out;
}

} // namespace

class FileBrowserTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void aFileIsReadAndTracedAcrossARename();
    void theWorkingTreeIsEditedInPlace();
    void aFileOpenInTheDiffOpensInTheFilesTab();

private:
    bool commit(const QString& message);
    FileBrowser* openFiles(MainWindow& window);

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
    QVERIFY(writeFile(m_path + "/crlf.txt", "first\r\nsecond\r\n"));
    QVERIFY(commit("create a"));
    QVERIFY(writeFile(m_path + "/a.txt", "one\ntwo changed\nthree\nfour\nfive\n"));
    QVERIFY(commit("change line two"));
    QVERIFY(QDir(m_path).mkpath("dir"));
    QVERIFY(git(m_path, {"mv", "a.txt", "dir/b.txt"}));
    QVERIFY(commit("move a to dir/b"));
    QVERIFY(writeFile(m_path + "/dir/b.txt", "one\ntwo changed\nthree\nfour\nfive changed\n"));
    QVERIFY(commit("change line five"));
}

void FileBrowserTest::cleanup() {
    m_dir.reset();
}

FileBrowser* FileBrowserTest::openFiles(MainWindow& window) {
    window.showFilesPageForDiagnostics();
    auto* tabs = window.findChild<QTabBar*>(QStringLiteral("centerTabs"));
    if (!tabs || tabs->currentIndex() != 1) {
        return nullptr;
    }
    return window.fileBrowserForDiagnostics();
}

void FileBrowserTest::aFileIsReadAndTracedAcrossARename() {
    MainWindow window;
    window.setHeadless(true);
    QVERIFY(window.openRepository(m_path));
    FileBrowser* files = openFiles(window);
    QVERIFY(files);
    // Nothing selected: the working tree.
    QVERIFY(files->workingTree());
    QTRY_VERIFY(!files->busy() && !files->treeRowsForDiagnostics().isEmpty());
    QCOMPARE(files->treeRowsForDiagnostics(),
             (QStringList{QStringLiteral("dir/"), QStringLiteral("  …"),
                          QStringLiteral("crlf.txt"), QStringLiteral("readme.md")}));
    files->expandForDiagnostics(QStringLiteral("dir"));
    QVERIFY(files->treeRowsForDiagnostics().contains(QStringLiteral("  b.txt")));

    files->openFile(QStringLiteral("dir/b.txt"));
    QTRY_VERIFY(!files->busy());
    QCOMPARE(files->viewerText(), QStringLiteral("one\ntwo changed\nthree\nfour\nfive changed\n"));
    QStringList history = files->historyRowsForDiagnostics();
    QVERIFY2(history.size() == 5, qPrintable(history.join('\n')));
    QVERIFY(history.at(4).endsWith(QStringLiteral("create a")));

    // The change before the rename is shown in the diff view, under the old name.
    files->showHistoryEntry(2);
    QTRY_VERIFY(!files->busy());
    QVERIFY(files->showingChanges());
    QCOMPARE(files->changesView()->document().path, QStringLiteral("a.txt"));
    QCOMPARE(addedLines(files->changesView()->document().diff),
             QStringList{QStringLiteral("+two changed")});
    files->changesView()->setSplitView(true);
    QCOMPARE(files->changesView()->splitPaneBlocks(0), files->changesView()->splitPaneBlocks(1));

    // Line two was written by the first commit and changed by the second, nothing else.
    files->showLineHistory(2, 2);
    QTRY_VERIFY(!files->busy() && files->historyRowsForDiagnostics().size() == 3);
    history = files->historyRowsForDiagnostics();
    QVERIFY2(history.at(1).endsWith(QStringLiteral("change line two")),
             qPrintable(history.join('\n')));
    QVERIFY(history.at(2).endsWith(QStringLiteral("create a")));
    files->showHistoryEntry(0);
    QTRY_VERIFY(!files->busy());
    QCOMPARE(addedLines(files->changesView()->document().diff),
             QStringList{QStringLiteral("+two changed")});

    // An older commit shows its own tree, read-only.
    files->setRevision(gitOut(m_path, {"rev-parse", "HEAD~2"}), QStringLiteral("old"));
    QTRY_VERIFY(!files->busy() && files->treeRowsForDiagnostics().contains(QStringLiteral("a.txt")));
    files->openFile(QStringLiteral("a.txt"));
    QTRY_VERIFY(!files->busy());
    QVERIFY(!files->editable());
    QVERIFY(files->editor()->isReadOnly());
    QVERIFY(files->viewerTitle().endsWith(QStringLiteral("read-only")));
}

void FileBrowserTest::theWorkingTreeIsEditedInPlace() {
    MainWindow window;
    window.setHeadless(true);
    QVERIFY(window.openRepository(m_path));
    FileBrowser* files = openFiles(window);
    QVERIFY(files && files->workingTree());

    files->openFile(QStringLiteral("readme.md"));
    QTRY_VERIFY(!files->busy());
    QVERIFY(files->editable());
    QVERIFY(!files->editor()->isReadOnly());
    QTextCursor cursor(files->editor()->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("more\n"));
    QVERIFY(files->modified());
    QVERIFY(files->save());
    QVERIFY(!files->modified());
    QCOMPARE(readFile(m_path + "/readme.md"), QByteArray("readme\nmore\n"));
    QCOMPARE(gitOut(m_path, {"status", "--porcelain"}), QStringLiteral("M readme.md"));

    // Windows line ends stay what they were.
    files->openFile(QStringLiteral("crlf.txt"));
    QTRY_VERIFY(!files->busy());
    QCOMPARE(files->viewerText(), QStringLiteral("first\nsecond\n"));
    cursor = QTextCursor(files->editor()->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("third\n"));
    // Leaving an edited file headless saves it.
    files->openFile(QStringLiteral("readme.md"));
    QTRY_VERIFY(!files->busy());
    QCOMPARE(readFile(m_path + "/crlf.txt"), QByteArray("first\r\nsecond\r\nthird\r\n"));
}

void FileBrowserTest::aFileOpenInTheDiffOpensInTheFilesTab() {
    QVERIFY(writeFile(m_path + "/readme.md", "changed\n"));
    MainWindow window;
    window.setHeadless(true);
    QVERIFY(window.openRepository(m_path));
    window.openFileForDiagnostics(QStringLiteral("readme.md"), true);
    FileBrowser* files = openFiles(window);
    QVERIFY(files);
    QCOMPARE(files->openPath(), QStringLiteral("readme.md"));
    QVERIFY(files->workingTree());
    QTRY_VERIFY(!files->busy());
    QCOMPARE(files->viewerText(), QStringLiteral("changed\n"));
}

QTEST_MAIN(FileBrowserTest)
#include "test_file_browser.moc"
