/// Staging hunks and lines, against real repositories. The oracle is git itself: the
/// patch this code renders is applied by the binary to a reset index, and the two
/// indexes must be the same.

#include "core/patch_selection.h"
#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

ProcessResult gitRun(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString());
}

bool git(const QString& dir, const QStringList& args) {
    return gitRun(dir, args).exitCode == 0;
}

QString gitOut(const QString& dir, const QStringList& args) {
    return QString::fromStdString(gitRun(dir, args).out);
}

bool writeFile(const QString& dir, const QString& name, const QByteArray& content) {
    const QString full = QDir(dir).filePath(name);
    QDir().mkpath(QFileInfo(full).absolutePath());
    QFile file(full);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content) == content.size();
}

QByteArray readFile(const QString& dir, const QString& name) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

RepoSession::HunkAnchor anchorOf(const FileDiff& diff, int hunkIndex) {
    RepoSession::HunkAnchor anchor;
    const DiffHunk& hunk = diff.hunks.at(static_cast<std::size_t>(hunkIndex));
    anchor.oldStart = hunk.oldStart;
    anchor.oldLines = hunk.oldLines;
    anchor.newStart = hunk.newStart;
    anchor.newLines = hunk.newLines;
    return anchor;
}

} // namespace

class StagingLinesTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

private slots:
    void init();
    void cleanup();

    void stagingAWholeHunkLeavesTheOtherUnstaged();
    void stagingOneAddedLine();
    void stagingOneRemovedLine();
    void stagingASelectionSplitsAHunk();
    void theGitBinaryAppliesTheSamePatchToTheSameIndex();
    void unstagingAHunkPutsItBackIntoTheWorkingTree();
    void discardingAHunkRestoresTheWorkingTree();
    void theWindowDiscardsAHunkThroughItsOwnPath();
    void stagingLinesOfANewFile();
    void aFileWithoutTrailingNewlineKeepsIt();
    void theWindowStagesAHunkThroughItsOwnPath();
};

void StagingLinesTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();

    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));

    QByteArray base;
    for (int i = 1; i <= 20; ++i) {
        base += "line" + QByteArray::number(i) + "\n";
    }
    QVERIFY(writeFile(m_path, "file.txt", base));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
}

void StagingLinesTest::cleanup() {
    m_dir.reset();
}

void StagingLinesTest::stagingAWholeHunkLeavesTheOtherUnstaged() {
    QByteArray modified = readFile(m_path, "file.txt");
    modified.replace("line2\n", "line2-changed\n");
    modified.replace("line18\n", "line18-changed\n");
    QVERIFY(writeFile(m_path, "file.txt", modified));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const FileDiff diff = session.workdirFileDiff("file.txt");
    QCOMPARE(diff.hunks.size(), std::size_t(2));

    QString error;
    QVERIFY2(session.applyHunkSelection("file.txt", 0, {}, RepoSession::HunkApply::Stage, anchorOf(diff, 0), &error),
             qPrintable(error));

    const QString staged = gitOut(m_path, {"show", ":file.txt"});
    QVERIFY(staged.contains("line2-changed"));
    QVERIFY(!staged.contains("line18-changed"));
    QVERIFY(gitOut(m_path, {"diff"}).contains("line18-changed"));
    QVERIFY(!gitOut(m_path, {"diff", "--cached"}).contains("line18-changed"));
}

void StagingLinesTest::stagingOneAddedLine() {
    QByteArray modified = readFile(m_path, "file.txt");
    modified.replace("line5\n", "line5\ninserted\n");
    QVERIFY(writeFile(m_path, "file.txt", modified));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const FileDiff diff = session.workdirFileDiff("file.txt");
    QCOMPARE(diff.hunks.size(), std::size_t(1));

    int addedIndex = -1;
    const std::vector<DiffLine>& lines = diff.hunks.front().lines;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].kind == DiffLineKind::Added) {
            addedIndex = static_cast<int>(i);
            break;
        }
    }
    QVERIFY(addedIndex >= 0);

    QString error;
    QVERIFY2(session.applyHunkSelection("file.txt", 0, {addedIndex}, RepoSession::HunkApply::Stage, anchorOf(diff, 0),
                                        &error),
             qPrintable(error));
    const QString staged = gitOut(m_path, {"show", ":file.txt"});
    QVERIFY(staged.contains("inserted"));
}

void StagingLinesTest::stagingOneRemovedLine() {
    QByteArray modified = readFile(m_path, "file.txt");
    modified.replace("line4\n", "");
    QVERIFY(writeFile(m_path, "file.txt", modified));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const FileDiff diff = session.workdirFileDiff("file.txt");
    QCOMPARE(diff.hunks.size(), std::size_t(1));

    int removedIndex = -1;
    const std::vector<DiffLine>& lines = diff.hunks.front().lines;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].kind == DiffLineKind::Removed) {
            removedIndex = static_cast<int>(i);
            break;
        }
    }
    QVERIFY(removedIndex >= 0);

    QString error;
    QVERIFY2(session.applyHunkSelection("file.txt", 0, {removedIndex}, RepoSession::HunkApply::Stage, anchorOf(diff, 0),
                                        &error),
             qPrintable(error));
    QVERIFY(!gitOut(m_path, {"show", ":file.txt"}).contains("line4"));
}

void StagingLinesTest::stagingASelectionSplitsAHunk() {
    // Three changes close enough that libgit2 folds them into one hunk. Keeping
    // the outer two and dropping the middle one forces the hunk to be split: a
    // hunk whose old-side lines are not consecutive is not a hunk.
    QByteArray modified = readFile(m_path, "file.txt");
    modified.replace("line4\n", "line4-A\n");
    modified.replace("line6\n", "line6-B\n");
    modified.replace("line8\n", "line8-C\n");
    QVERIFY(writeFile(m_path, "file.txt", modified));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const FileDiff diff = session.workdirFileDiff("file.txt");
    QCOMPARE(diff.hunks.size(), std::size_t(1));

    QVector<int> wanted;
    const std::vector<DiffLine>& lines = diff.hunks.front().lines;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].kind == DiffLineKind::Context) {
            continue;
        }
        const int at = lines[i].oldLine > 0 ? lines[i].oldLine : lines[i].newLine;
        if (at == 4 || at == 8) {
            wanted.push_back(static_cast<int>(i));
        }
    }
    QVERIFY(!wanted.isEmpty());

    QString error;
    QVERIFY2(session.applyHunkSelection("file.txt", 0, wanted, RepoSession::HunkApply::Stage, anchorOf(diff, 0), &error),
             qPrintable(error));

    const QString staged = gitOut(m_path, {"show", ":file.txt"});
    QVERIFY(staged.contains("line4-A"));
    QVERIFY(staged.contains("line8-C"));
    QVERIFY(!staged.contains("line6-B"));
    QVERIFY(gitOut(m_path, {"diff"}).contains("line6-B"));
}

void StagingLinesTest::theGitBinaryAppliesTheSamePatchToTheSameIndex() {
    QByteArray modified = readFile(m_path, "file.txt");
    modified.replace("line2\n", "line2-changed\n");
    modified.replace("line18\n", "line18-changed\n");
    QVERIFY(writeFile(m_path, "file.txt", modified));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const FileDiff diff = session.workdirFileDiff("file.txt");
    QCOMPARE(diff.hunks.size(), std::size_t(2));

    const std::string patch = renderPatch(diff, {StageSelection{0, {}}}, false);

    QString error;
    QVERIFY2(session.applyHunkSelection("file.txt", 0, {}, RepoSession::HunkApply::Stage, anchorOf(diff, 0), &error),
             qPrintable(error));
    const QString stagedByUs = gitOut(m_path, {"show", ":file.txt"});

    // Put the index back and apply the very same patch through the git binary.
    QVERIFY(git(m_path, {"reset", "-q"}));
    QVERIFY(writeFile(m_path, "applied.patch", QByteArray::fromStdString(patch)));
    const ProcessResult applied = gitRun(m_path, {"apply", "--cached", "applied.patch"});
    QVERIFY2(applied.exitCode == 0, applied.out.c_str());
    const QString stagedByGit = gitOut(m_path, {"show", ":file.txt"});

    QCOMPARE(stagedByUs, stagedByGit);
}

void StagingLinesTest::unstagingAHunkPutsItBackIntoTheWorkingTree() {
    QByteArray modified = readFile(m_path, "file.txt");
    modified.replace("line2\n", "line2-changed\n");
    modified.replace("line18\n", "line18-changed\n");
    QVERIFY(writeFile(m_path, "file.txt", modified));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.stageFile("file.txt", &error), qPrintable(error));

    const FileDiff staged = session.stagedFileDiff("file.txt");
    QCOMPARE(staged.hunks.size(), std::size_t(2));

    QVERIFY2(session.applyHunkSelection("file.txt", 0, {}, RepoSession::HunkApply::Unstage,
                                        anchorOf(staged, 0), &error),
             qPrintable(error));

    QVERIFY(gitOut(m_path, {"show", ":file.txt"}).contains("line18-changed"));
    QVERIFY(!gitOut(m_path, {"show", ":file.txt"}).contains("line2-changed"));
    QVERIFY(gitOut(m_path, {"diff"}).contains("line2-changed"));
    QVERIFY(!gitOut(m_path, {"diff", "--cached"}).contains("line2-changed"));
}

void StagingLinesTest::discardingAHunkRestoresTheWorkingTree() {
    QByteArray modified = readFile(m_path, "file.txt");
    modified.replace("line2\n", "line2-changed\n");
    modified.replace("line18\n", "line18-changed\n");
    QVERIFY(writeFile(m_path, "file.txt", modified));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const FileDiff diff = session.workdirFileDiff("file.txt");
    QCOMPARE(diff.hunks.size(), std::size_t(2));

    QString error;
    QVERIFY2(session.applyHunkSelection("file.txt", 0, {}, RepoSession::HunkApply::Discard,
                                        anchorOf(diff, 0), &error),
             qPrintable(error));

    // The first hunk is gone from the working tree; the second is untouched, and the
    // index (nothing staged) is untouched: discarding is a working-tree operation.
    const QByteArray now = readFile(m_path, "file.txt");
    QVERIFY(!now.contains("line2-changed"));
    QVERIFY(now.contains("line2\n"));
    QVERIFY(now.contains("line18-changed"));
    QVERIFY(gitOut(m_path, {"diff"}).contains("line18-changed"));
    QVERIFY(!gitOut(m_path, {"diff"}).contains("line2-changed"));
    QVERIFY(gitOut(m_path, {"diff", "--cached"}).isEmpty());
}

void StagingLinesTest::theWindowDiscardsAHunkThroughItsOwnPath() {
    QByteArray modified = readFile(m_path, "file.txt");
    modified.replace("line2\n", "line2-changed\n");
    modified.replace("line18\n", "line18-changed\n");
    QVERIFY(writeFile(m_path, "file.txt", modified));

    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QStringList discarded =
        window.discardHunkForDiagnostics(QStringLiteral("file.txt"), 0, -1);
    const QString text = discarded.join(QLatin1Char('\n'));
    QVERIFY2(text.contains(QStringLiteral("discarded hunk 0")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("unstaged diff: 1 hunk(s)")));

    const QByteArray now = readFile(m_path, "file.txt");
    QVERIFY(!now.contains("line2-changed"));
    QVERIFY(now.contains("line18-changed"));
}

void StagingLinesTest::stagingLinesOfANewFile() {
    QVERIFY(writeFile(m_path, "new.txt", "alpha\nbeta\ngamma\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const FileDiff diff = session.workdirFileDiff("new.txt");
    QCOMPARE(diff.hunks.size(), std::size_t(1));

    QString error;
    QVERIFY2(session.applyHunkSelection("new.txt", 0, {0}, RepoSession::HunkApply::Stage, anchorOf(diff, 0), &error),
             qPrintable(error));

    const QString staged = gitOut(m_path, {"show", ":new.txt"});
    QVERIFY(staged.contains("alpha"));
    QVERIFY(!staged.contains("gamma"));
    QVERIFY(readFile(m_path, "new.txt").contains("gamma"));
}

void StagingLinesTest::aFileWithoutTrailingNewlineKeepsIt() {
    QVERIFY(writeFile(m_path, "noeol.txt", "first\nsecond"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "noeol"}));

    QVERIFY(writeFile(m_path, "noeol.txt", "first\nsecond-changed"));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const FileDiff diff = session.workdirFileDiff("noeol.txt");
    QCOMPARE(diff.hunks.size(), std::size_t(1));

    QString error;
    QVERIFY2(session.applyHunkSelection("noeol.txt", 0, {}, RepoSession::HunkApply::Stage, anchorOf(diff, 0), &error),
             qPrintable(error));
    const QString staged = gitOut(m_path, {"show", ":noeol.txt"});
    QVERIFY(staged.contains("second-changed"));
    QVERIFY(!staged.endsWith("\n"));
}

void StagingLinesTest::theWindowStagesAHunkThroughItsOwnPath() {
    QByteArray modified = readFile(m_path, "file.txt");
    modified.replace("line2\n", "line2-changed\n");
    modified.replace("line18\n", "line18-changed\n");
    QVERIFY(writeFile(m_path, "file.txt", modified));

    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QStringList staged = window.stageHunkForDiagnostics(QStringLiteral("file.txt"), 0, false);
    const QString stagedText = staged.join(QLatin1Char('\n'));
    QVERIFY2(stagedText.contains(QStringLiteral("moved hunk 0")), qPrintable(stagedText));
    QVERIFY(gitOut(m_path, {"show", ":file.txt"}).contains("line2-changed"));
    QVERIFY(!gitOut(m_path, {"show", ":file.txt"}).contains("line18-changed"));

    const QStringList taken = window.stageHunkForDiagnostics(QStringLiteral("file.txt"), 0, true);
    QVERIFY2(taken.join(QLatin1Char('\n')).contains(QStringLiteral("moved hunk 0")),
             qPrintable(taken.join(QLatin1Char('\n'))));
    QVERIFY(!gitOut(m_path, {"show", ":file.txt"}).contains("line2-changed"));
}

QTEST_MAIN(StagingLinesTest)
#include "test_staging_lines.moc"
