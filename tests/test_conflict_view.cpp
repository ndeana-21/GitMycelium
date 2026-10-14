#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/conflict_view.h"
#include "ui/main_window.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QToolButton>
#include <QFrame>
#include <QPushButton>
#include <QTreeWidget>
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
    return QString::fromStdString(runGit(argv, dir.toStdString()).out);
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

QString readFile(const QString& dir, const QString& name) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

QByteArray readRaw(const QString& dir, const QString& name) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

/// Bytes that make git call the file binary: a NUL in the first few bytes.
QByteArray bin(const char* text) {
    QByteArray bytes;
    bytes.append(char(0x00));
    bytes.append(char(0x01));
    bytes.append(char(0x02));
    bytes.append(text);
    bytes.append(char(0xff));
    return bytes;
}

const QByteArray kBaseBin = bin("base");
const QByteArray kMainBin = bin("MAIN");
const QByteArray kFeatureBin = bin("FEATURE");

const QStringList kIdentity{"-c", "user.name=Test", "-c", "user.email=test@localhost"};

} // namespace

/// The per-hunk merge editor, against a repository git itself put into conflict.
/// The point is that taking one side for a region and saving is exactly what git
/// gets when a person edits the file and stages it.
class ConflictViewTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void regionsAreOfferedAndAssembledFromChoices();
    void bothOrdersConcatenateTheSides();
    void takeBaseRestoresTheCommonAncestor();
    void anUnresolvedRegionKeepsItsMarkersAndRefusesToSave();
    void saveWritesAndStagesTheResult();
    void selectFileRejectsAnUnknownPath();
    void regionsReportWordEmphasis();
    void theWindowDrivesTheEditorHeadlessly();
    void closingTheMergeEditorLeavesItClosed();
    void theWindowDrivesPerLineResolutionHeadlessly();
    void aBinaryConflictIsResolvedByKeepingASide();
    void aDisjointJsonConflictMergesItself();
    void aJsonConflictOnOneKeyKeepsThatRegion();
    void stagingTheWorkingTreeFileFinishesAnExternalResolution();
    void markAllResolvedStagesTheResolvedFilesAndNamesTheRest();
    void perLineTakesChosenLinesFromEachSide();
    void droppingEveryLineLeavesTheRegionUnresolved();
    void aDeleteModifyConflictIsSettledByDeletingTheFile();
    void keepingTheModifiedSideOfADeleteModifyKeepsTheFile();
    void choosingTheDeletedSideRemovesTheFileInsteadOfWritingItEmpty();
    void aDeleteModifyIsAnsweredOneOfFourWays_data();
    void aDeleteModifyIsAnsweredOneOfFourWays();
    void deletedOnOurSideKeepsTheirChanges();
    void theQuestionIsAskedOnlyOfADeleteModify();
    void aResolvedFileCanBeMarkedConflictedAgain();
    void clearingChoicesLeavesEveryRegionUnanswered();
    void theResolvedListEndsWithTheOperation();
    void aDraftIsSavedWithItsMarkersAndNothingStaged();

private:
    void buildConflict();
    void buildMultiLineConflict(const QString& dir);
    void buildDeleteModifyConflict(const QString& dir);
    void buildBinaryConflict(const QString& dir);
    void buildJsonConflict(const QString& dir, const QString& ours, const QString& theirs);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void ConflictViewTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    buildConflict();
}

void ConflictViewTest::cleanup() {
    m_dir.reset();
}

void ConflictViewTest::buildConflict() {
    QVERIFY(writeFile(m_path, "a.txt", "line1\nline2\nline3\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, kIdentity + QStringList{"commit", "-q", "-m", "base"}));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nFEATURE\nline3\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, kIdentity + QStringList{"commit", "-q", "-m", "feature"}));

    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nMAIN\nline3\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, kIdentity + QStringList{"commit", "-q", "-m", "main"}));

    // The merge stops on the same line changed two ways; git leaves the index
    // conflicted, which is the state the editor is for.
    git(m_path, kIdentity + QStringList{"merge", "feature"});
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).contains("a.txt"));
}

void ConflictViewTest::buildMultiLineConflict(const QString& dir) {
    QVERIFY(git(dir, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(dir, "m.txt", "head\nB\nC\ntail\n"));
    QVERIFY(git(dir, {"add", "m.txt"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "base multiline"}));

    QVERIFY(git(dir, {"checkout", "-q", "-b", "feature-multi"}));
    QVERIFY(writeFile(dir, "m.txt", "head\nT1\nT2\ntail\n"));
    QVERIFY(git(dir, {"add", "m.txt"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "feature multiline"}));

    QVERIFY(git(dir, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(dir, "m.txt", "head\nO1\nO2\ntail\n"));
    QVERIFY(git(dir, {"add", "m.txt"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "main multiline"}));

    git(dir, kIdentity + QStringList{"merge", "feature-multi"});
    QVERIFY(gitOut(dir, {"ls-files", "-u"}).contains("m.txt"));
}

void ConflictViewTest::buildDeleteModifyConflict(const QString& dir) {
    QVERIFY(git(dir, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(dir, "d.txt", "keep me\n"));
    QVERIFY(git(dir, {"add", "d.txt"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "base"}));

    QVERIFY(git(dir, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(git(dir, {"rm", "-q", "d.txt"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "delete d"}));

    QVERIFY(git(dir, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(dir, "d.txt", "modified content\n"));
    QVERIFY(git(dir, {"add", "d.txt"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "modify d"}));

    git(dir, kIdentity + QStringList{"merge", "feature"});
    QVERIFY(gitOut(dir, {"ls-files", "-u"}).contains("d.txt"));
}

void ConflictViewTest::buildBinaryConflict(const QString& dir) {
    QVERIFY(git(dir, {"init", "-q", "-b", "main"}));
    const auto write = [&dir](const QByteArray& bytes) {
        QFile file(QDir(dir).filePath(QStringLiteral("bin.dat")));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(bytes) == bytes.size());
    };
    write(kBaseBin);
    QVERIFY(git(dir, {"add", "bin.dat"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "base"}));

    QVERIFY(git(dir, {"checkout", "-q", "-b", "feature"}));
    write(kFeatureBin);
    QVERIFY(git(dir, {"add", "bin.dat"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "feature"}));

    QVERIFY(git(dir, {"checkout", "-q", "main"}));
    write(kMainBin);
    QVERIFY(git(dir, {"add", "bin.dat"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "main"}));

    git(dir, kIdentity + QStringList{"merge", "feature"});
    QVERIFY(gitOut(dir, {"ls-files", "-u"}).contains("bin.dat"));
}

void ConflictViewTest::buildJsonConflict(const QString& dir, const QString& ours,
                                         const QString& theirs) {
    QVERIFY(git(dir, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(dir, "package.json", QStringLiteral("{\"a\":1,\"b\":1}\n")));
    QVERIFY(git(dir, {"add", "package.json"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "base"}));

    QVERIFY(git(dir, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(dir, "package.json", theirs));
    QVERIFY(git(dir, {"add", "package.json"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "feature"}));

    QVERIFY(git(dir, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(dir, "package.json", ours));
    QVERIFY(git(dir, {"add", "package.json"}));
    QVERIFY(git(dir, kIdentity + QStringList{"commit", "-q", "-m", "main"}));

    git(dir, kIdentity + QStringList{"merge", "feature"});
    QVERIFY(gitOut(dir, {"ls-files", "-u"}).contains("package.json"));
}

void ConflictViewTest::regionsAreOfferedAndAssembledFromChoices() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();

    QCOMPARE(view.conflictCount(), 1);
    QCOMPARE(view.currentPath(), QStringLiteral("a.txt"));
    QCOMPARE(view.regionCount(), 1);
    QCOMPARE(view.unresolvedRegionCount(), 1);

    QVERIFY(view.setRegionChoice(0, "ours"));
    QCOMPARE(view.unresolvedRegionCount(), 0);
    QCOMPARE(view.assembledText(), QStringLiteral("line1\nMAIN\nline3\n"));
}

void ConflictViewTest::bothOrdersConcatenateTheSides() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();

    QVERIFY(view.setRegionChoice(0, "both"));
    QCOMPARE(view.assembledText(), QStringLiteral("line1\nMAIN\nFEATURE\nline3\n"));

    QVERIFY(view.setRegionChoice(0, "both-rev"));
    QCOMPARE(view.assembledText(), QStringLiteral("line1\nFEATURE\nMAIN\nline3\n"));
}

void ConflictViewTest::takeBaseRestoresTheCommonAncestor() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();

    QVERIFY(view.setRegionChoice(0, "base"));
    QCOMPARE(view.assembledText(), QStringLiteral("line1\nline2\nline3\n"));
}

void ConflictViewTest::anUnresolvedRegionKeepsItsMarkersAndRefusesToSave() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();

    QVERIFY(view.assembledText().contains(QStringLiteral("<<<<<<<")));
    QString error;
    QVERIFY(!view.trySaveResolution(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!session.conflicts().empty());
}

void ConflictViewTest::saveWritesAndStagesTheResult() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();

    QVERIFY(view.setRegionChoice(0, "theirs"));
    QString error;
    QVERIFY2(view.trySaveResolution(&error), qPrintable(error));

    QVERIFY(session.conflicts().empty());
    QCOMPARE(readFile(m_path, "a.txt"), QStringLiteral("line1\nFEATURE\nline3\n"));
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::selectFileRejectsAnUnknownPath() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();

    QVERIFY(!view.selectFile(QStringLiteral("nope.txt")));
}

void ConflictViewTest::regionsReportWordEmphasis() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();

    // The conflicting line is "MAIN" against the base "line2": no token survives, so
    // the whole line is emphasised on each side. The headless report carries the
    // counts, which is how this is asserted without reading HTML.
    const QStringList regions = view.describeRegions();
    QCOMPARE(regions.size(), 1);
    QVERIFY2(regions.first().contains(QStringLiteral("oursEmph=1")), qPrintable(regions.first()));
    QVERIFY(regions.first().contains(QStringLiteral("theirsEmph=1")));
}

void ConflictViewTest::theWindowDrivesTheEditorHeadlessly() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QStringList regions = window.conflictForDiagnostics(QStringLiteral("regions"), {});
    const QString regionText = regions.join(QLatin1Char('\n'));
    QVERIFY2(regionText.contains(QStringLiteral("regions: 1")), qPrintable(regionText));
    QVERIFY(regionText.contains(QStringLiteral("unresolved: 1")));

    const QStringList take =
        window.conflictForDiagnostics(QStringLiteral("take"), {QStringLiteral("0"),
                                                               QStringLiteral("theirs")});
    QVERIFY2(take.join(QLatin1Char('\n')).contains(QStringLiteral("take: ok")),
             qPrintable(take.join(QLatin1Char('\n'))));

    const QStringList save = window.conflictForDiagnostics(QStringLiteral("save"), {});
    const QString saveText = save.join(QLatin1Char('\n'));
    QVERIFY2(saveText.contains(QStringLiteral("save: ok")), qPrintable(saveText));
    QVERIFY(saveText.contains(QStringLiteral("conflicts left: 0")));
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::closingTheMergeEditorLeavesItClosed() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    // The window opens the page itself while the operation is unfinished, and the
    // reason is right: the Completing controls live here and would otherwise vanish
    // the moment the last file is resolved.
    QStringList report = window.conflictForDiagnostics(QStringLiteral("regions"), {});
    QVERIFY2(report.join(QLatin1Char('\n')).contains(QStringLiteral("page: CONFLICT")),
             qPrintable(report.join(QLatin1Char('\n'))));

    // The button the reader presses. Testing the page rather than the signal: a
    // signal that fires is not the same claim as a button that works.
    auto* close = window.findChild<QToolButton*>(QStringLiteral("conflictClose"));
    QVERIFY2(close, "the merge editor has to have a way out");
    close->click();

    report = window.conflictForDiagnostics(QStringLiteral("regions"), {});
    QVERIFY2(report.join(QLatin1Char('\n')).contains(QStringLiteral("page: history")),
             qPrintable(report.join(QLatin1Char('\n'))));
    QVERIFY(report.join(QLatin1Char('\n')).contains(QStringLiteral("conflicts left: 1")));

    // A refresh used to drag the reader straight back, because the page is sticky
    // while `mergeInProgress()`. That stickiness is what the reader has now answered.
    report = window.conflictForDiagnostics(QStringLiteral("refresh"), {});
    QVERIFY2(report.join(QLatin1Char('\n')).contains(QStringLiteral("page: history")),
             qPrintable(report.join(QLatin1Char('\n'))));

    window.commandPaletteForDiagnostics(QStringLiteral("Show Conflicts"), true);
    report = window.conflictForDiagnostics(QStringLiteral("regions"), {});
    QVERIFY2(report.join(QLatin1Char('\n')).contains(QStringLiteral("page: CONFLICT")),
             qPrintable(report.join(QLatin1Char('\n'))));
}

void ConflictViewTest::aBinaryConflictIsResolvedByKeepingASide() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildBinaryConflict(dir.path());

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();

    QCOMPARE(view.conflictCount(), 1);
    QVERIFY2(view.isBinary(), "a binary file has no text to merge");
    QCOMPARE(view.regionCount(), 0);

    // Saving text would stage an empty file over the binary; the text path refuses.
    QString error;
    QVERIFY(!view.trySaveResolution(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!session.conflicts().empty());

    QVERIFY2(view.keepSide(QStringLiteral("theirs"), &error), qPrintable(error));
    QCOMPARE(readRaw(dir.path(), QStringLiteral("bin.dat")), kFeatureBin);
    QVERIFY(gitOut(dir.path(), {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::aDisjointJsonConflictMergesItself() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildJsonConflict(dir.path(), QStringLiteral("{\"a\":2,\"b\":1}\n"),
                      QStringLiteral("{\"a\":1,\"b\":3}\n"));

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();

    QCOMPARE(view.conflictCount(), 1);
    QString error;
    QVERIFY2(view.smartMerge(&error), qPrintable(error));
    QCOMPARE(view.regionCount(), 0);
    QCOMPARE(view.resultText(), QStringLiteral("{\"a\":2,\"b\":3}\n"));

    QVERIFY2(view.trySaveResolution(&error), qPrintable(error));
    QVERIFY(session.conflicts().empty());
    QCOMPARE(readFile(dir.path(), "package.json"), QStringLiteral("{\"a\":2,\"b\":3}\n"));
    QVERIFY(gitOut(dir.path(), {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::aJsonConflictOnOneKeyKeepsThatRegion() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildJsonConflict(dir.path(), QStringLiteral("{\"a\":2,\"b\":1}\n"),
                      QStringLiteral("{\"a\":3,\"b\":1}\n"));

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();

    QString error;
    QVERIFY2(view.smartMerge(&error), qPrintable(error));
    QCOMPARE(view.regionCount(), 1);
    QVERIFY(view.setRegionChoice(0, "ours"));
    QVERIFY2(view.trySaveResolution(&error), qPrintable(error));
    QCOMPARE(readFile(dir.path(), "package.json"), QStringLiteral("{\n\"a\":2,\n\"b\":1}\n"));
}

void ConflictViewTest::stagingTheWorkingTreeFileFinishesAnExternalResolution() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();

    QCOMPARE(view.currentKind(), QStringLiteral("both modified"));

    QString error;
    QVERIFY(!view.stageWorkingTree(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!session.conflicts().empty());

    QVERIFY(writeFile(m_path, "a.txt", "line1\nEXTERNAL\nline3\n"));
    QVERIFY2(view.stageWorkingTree(&error), qPrintable(error));
    QVERIFY(session.conflicts().empty());
    QCOMPARE(readFile(m_path, "a.txt"), QStringLiteral("line1\nEXTERNAL\nline3\n"));
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::markAllResolvedStagesTheResolvedFilesAndNamesTheRest() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString d = dir.path();
    QVERIFY(git(d, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(d, "a.txt", "a1\n"));
    QVERIFY(writeFile(d, "b.txt", "b1\n"));
    QVERIFY(git(d, {"add", "a.txt", "b.txt"}));
    QVERIFY(git(d, kIdentity + QStringList{"commit", "-q", "-m", "base"}));
    QVERIFY(git(d, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(d, "a.txt", "a-feature\n"));
    QVERIFY(writeFile(d, "b.txt", "b-feature\n"));
    QVERIFY(git(d, {"add", "a.txt", "b.txt"}));
    QVERIFY(git(d, kIdentity + QStringList{"commit", "-q", "-m", "feature"}));
    QVERIFY(git(d, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(d, "a.txt", "a-main\n"));
    QVERIFY(writeFile(d, "b.txt", "b-main\n"));
    QVERIFY(git(d, {"add", "a.txt", "b.txt"}));
    QVERIFY(git(d, kIdentity + QStringList{"commit", "-q", "-m", "main"}));
    git(d, kIdentity + QStringList{"merge", "feature"});
    const QString unmerged = gitOut(d, {"ls-files", "-u"});
    QVERIFY(unmerged.contains(QStringLiteral("a.txt")));
    QVERIFY(unmerged.contains(QStringLiteral("b.txt")));

    RepoSession session;
    QVERIFY(session.open(d));
    ConflictView view(&session);
    view.reload();
    QCOMPARE(view.conflictCount(), 2);

    QVERIFY(writeFile(d, "a.txt", "a-resolved\n"));
    QStringList skipped;
    QString error;
    QVERIFY(view.markAllResolved(&skipped, &error));
    QCOMPARE(skipped, QStringList{QStringLiteral("b.txt")});

    QCOMPARE(session.conflicts().size(), static_cast<std::size_t>(1));
    QCOMPARE(QString::fromStdString(session.conflicts().front().path), QStringLiteral("b.txt"));
    QVERIFY(gitOut(d, {"ls-files", "-u", "a.txt"}).trimmed().isEmpty());
}

void ConflictViewTest::perLineTakesChosenLinesFromEachSide() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildMultiLineConflict(dir.path());

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();

    QCOMPARE(view.regionCount(), 1);
    QCOMPARE(view.unresolvedRegionCount(), 1);

    // Ours is {O1,O2}, theirs is {T1,T2}; take ours' second line and theirs' first.
    QVERIFY(view.setRegionLine(0, QStringLiteral("ours"), 1, true));
    QVERIFY(view.setRegionLine(0, QStringLiteral("theirs"), 0, true));
    QCOMPARE(view.unresolvedRegionCount(), 0);
    QCOMPARE(view.assembledText(), QStringLiteral("head\nO2\nT1\ntail\n"));

    QString error;
    QVERIFY2(view.trySaveResolution(&error), qPrintable(error));
    QVERIFY(session.conflicts().empty());
    QCOMPARE(readFile(dir.path(), "m.txt"), QStringLiteral("head\nO2\nT1\ntail\n"));
    QVERIFY(gitOut(dir.path(), {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::droppingEveryLineLeavesTheRegionUnresolved() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildMultiLineConflict(dir.path());

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();

    QVERIFY(view.setRegionLine(0, QStringLiteral("ours"), 0, true));
    QCOMPARE(view.unresolvedRegionCount(), 0);

    // Taking the last line away is not an answer: the region must open again and a
    // save must keep the markers rather than write half a file.
    QVERIFY(view.setRegionLine(0, QStringLiteral("ours"), 0, false));
    QCOMPARE(view.unresolvedRegionCount(), 1);
    QVERIFY(view.assembledText().contains(QStringLiteral("<<<<<<<")));
    QString error;
    QVERIFY(!view.trySaveResolution(&error));
}

void ConflictViewTest::theWindowDrivesPerLineResolutionHeadlessly() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildMultiLineConflict(dir.path());

    MainWindow window;
    QVERIFY(window.openRepository(dir.path()));

    const QStringList take = window.conflictForDiagnostics(
        QStringLiteral("line"), {QStringLiteral("0"), QStringLiteral("ours"),
                                 QStringLiteral("1")});
    QVERIFY2(take.join(QLatin1Char('\n')).contains(QStringLiteral("line: ok")),
             qPrintable(take.join(QLatin1Char('\n'))));

    const QStringList other = window.conflictForDiagnostics(
        QStringLiteral("takelines"), {QStringLiteral("0"), QStringLiteral("theirs")});
    QVERIFY(other.join(QLatin1Char('\n')).contains(QStringLiteral("takelines: ok")));

    const QStringList regions = window.conflictForDiagnostics(QStringLiteral("regions"), {});
    const QString regionText = regions.join(QLatin1Char('\n'));
    QVERIFY2(regionText.contains(QStringLiteral("unresolved: 0")), qPrintable(regionText));
    QVERIFY(regionText.contains(QStringLiteral("takenOurs=1")));
    QVERIFY(regionText.contains(QStringLiteral("takenTheirs=2")));

    const QStringList save = window.conflictForDiagnostics(QStringLiteral("save"), {});
    QVERIFY(save.join(QLatin1Char('\n')).contains(QStringLiteral("save: ok")));
    QCOMPARE(readFile(dir.path(), "m.txt"), QStringLiteral("head\nO2\nT1\nT2\ntail\n"));
    QVERIFY(gitOut(dir.path(), {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::aDeleteModifyConflictIsSettledByDeletingTheFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildDeleteModifyConflict(dir.path());

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();

    QCOMPARE(view.conflictCount(), 1);
    QVERIFY2(view.isDeleteModify(), "deleted-by-them is a delete/modify conflict");

    // Saving text would stage an empty file over the deletion; the explicit delete
    // is the honest answer.
    QString error;
    QVERIFY2(view.deleteCurrentFile(&error), qPrintable(error));
    QVERIFY(session.conflicts().empty());
    QVERIFY(!QFile::exists(QDir(dir.path()).filePath("d.txt")));
    QVERIFY(gitOut(dir.path(), {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::keepingTheModifiedSideOfADeleteModifyKeepsTheFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildDeleteModifyConflict(dir.path());

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();

    QCOMPARE(view.regionCount(), 1);
    QVERIFY(view.setRegionChoice(0, QStringLiteral("ours")));
    QString error;
    QVERIFY2(view.trySaveResolution(&error), qPrintable(error));
    QVERIFY(session.conflicts().empty());
    QCOMPARE(readFile(dir.path(), "d.txt"), QStringLiteral("modified content\n"));
}

void ConflictViewTest::choosingTheDeletedSideRemovesTheFileInsteadOfWritingItEmpty() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildDeleteModifyConflict(dir.path());

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();

    // "theirs" is the branch that deleted the file, so its stage is empty. Choosing
    // it must remove the file, not stage a zero-byte one.
    QVERIFY(view.setRegionChoice(0, QStringLiteral("theirs")));
    QString error;
    QVERIFY2(view.trySaveResolution(&error), qPrintable(error));
    QVERIFY(session.conflicts().empty());
    QVERIFY2(!QFile::exists(QDir(dir.path()).filePath("d.txt")),
             "the deleted side must remove the file, not leave an empty one");
    QVERIFY(gitOut(dir.path(), {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::aDeleteModifyIsAnsweredOneOfFourWays_data() {
    QTest::addColumn<QString>("answer");
    QTest::addColumn<bool>("kept");
    QTest::addColumn<QString>("content");
    QTest::newRow("modified") << QStringLiteral("modified") << true
                              << QStringLiteral("modified content\n");
    QTest::newRow("base") << QStringLiteral("base") << true << QStringLiteral("keep me\n");
    QTest::newRow("disk") << QStringLiteral("disk") << true
                          << QStringLiteral("edited on disk\n");
    QTest::newRow("delete") << QStringLiteral("delete") << false << QString();
}

void ConflictViewTest::aDeleteModifyIsAnsweredOneOfFourWays() {
    QFETCH(QString, answer);
    QFETCH(bool, kept);
    QFETCH(QString, content);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildDeleteModifyConflict(dir.path());
    // The reader went on editing the file after the merge stopped; only "disk"
    // keeps that edit.
    QVERIFY(writeFile(dir.path(), "d.txt", "edited on disk\n"));

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();
    QCOMPARE(view.deleteModifyLivingSide(), QStringLiteral("ours"));

    QString error;
    QVERIFY2(view.resolveDeleteModify(answer, &error), qPrintable(error));
    QVERIFY(session.conflicts().empty());
    QCOMPARE(view.conflictCount(), 0);
    QVERIFY(gitOut(dir.path(), {"ls-files", "-u"}).trimmed().isEmpty());
    QCOMPARE(QFile::exists(QDir(dir.path()).filePath("d.txt")), kept);
    QCOMPARE(gitOut(dir.path(), {"ls-files", "d.txt"}).trimmed().isEmpty(), !kept);
    if (kept) {
        QCOMPARE(readFile(dir.path(), "d.txt"), content);
        QCOMPARE(gitOut(dir.path(), {"diff", "--name-only", "--", "d.txt"}).trimmed(), QString());
    }
}

void ConflictViewTest::deletedOnOurSideKeepsTheirChanges() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildDeleteModifyConflict(dir.path());
    QVERIFY(git(dir.path(), {"merge", "--abort"}));
    QVERIFY(git(dir.path(), {"checkout", "-q", "feature"}));
    git(dir.path(), kIdentity + QStringList{"merge", "main"});
    QVERIFY(gitOut(dir.path(), {"ls-files", "-u"}).contains("d.txt"));

    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();
    QCOMPARE(view.deleteModifyLivingSide(), QStringLiteral("theirs"));
    QString error;
    QVERIFY2(view.resolveDeleteModify(QStringLiteral("modified"), &error), qPrintable(error));
    QCOMPARE(readFile(dir.path(), "d.txt"), QStringLiteral("modified content\n"));
    QVERIFY(gitOut(dir.path(), {"ls-files", "-u"}).trimmed().isEmpty());
}

void ConflictViewTest::theQuestionIsAskedOnlyOfADeleteModify() {
    {
        // init() left an ordinary both-modified conflict in m_path.
        RepoSession session;
        QVERIFY(session.open(m_path));
        ConflictView view(&session);
        view.reload();
        auto* panel = view.findChild<QFrame*>(QStringLiteral("deleteModifyPanel"));
        QVERIFY(panel);
        QVERIFY(panel->isHidden());
        QString error;
        QVERIFY(!view.resolveDeleteModify(QStringLiteral("base"), &error));
        QVERIFY(error.contains(QStringLiteral("not a delete/modify")));
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    buildDeleteModifyConflict(dir.path());
    RepoSession session;
    QVERIFY(session.open(dir.path()));
    ConflictView view(&session);
    view.reload();
    auto* panel = view.findChild<QFrame*>(QStringLiteral("deleteModifyPanel"));
    QVERIFY(panel);
    QVERIFY(!panel->isHidden());
    // In the editor's layout, above the panes — not a stray child floating at the
    // view's corner, which is what it was when it was laid out before it existed.
    view.resize(1200, 800);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QVERIFY(panel->mapTo(&view, QPoint(0, 0)).y() > 40);
    QVERIFY(panel->width() > 400);
    auto* keepBase = view.findChild<QPushButton*>(QStringLiteral("deleteModifyKeepBase"));
    QVERIFY(keepBase && keepBase->isEnabled());
    keepBase->click();
    QVERIFY(session.conflicts().empty());
    QCOMPARE(readFile(dir.path(), "d.txt"), QStringLiteral("keep me\n"));
    QVERIFY(panel->isHidden());
}

void ConflictViewTest::clearingChoicesLeavesEveryRegionUnanswered() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();
    const QString untouched = view.assembledText();
    QVERIFY(view.setRegionChoice(0, QStringLiteral("theirs")));
    QVERIFY(view.assembledText() != untouched);

    auto* clear = view.findChild<QPushButton*>(QStringLiteral("clearChoices"));
    QVERIFY(clear);
    clear->click();
    QCOMPARE(view.unresolvedRegionCount(), 1);
    QCOMPARE(view.assembledText(), untouched);
    QVERIFY(readFile(m_path, "a.txt").contains(QStringLiteral("<<<<<<<")));
}

void ConflictViewTest::aResolvedFileCanBeMarkedConflictedAgain() {
    const QString stagesBefore = gitOut(m_path, {"ls-files", "-u"});
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();
    QVERIFY(view.resolvedPaths().isEmpty());

    QVERIFY(view.setRegionChoice(0, QStringLiteral("ours")));
    QString error;
    QVERIFY2(view.trySaveResolution(&error), qPrintable(error));
    view.reload();
    QCOMPARE(view.conflictCount(), 0);
    // Staged the way `git add` stages: git itself can see the way back.
    QVERIFY(gitOut(m_path, {"ls-files", "--resolve-undo"}).contains(QStringLiteral("a.txt")));
    QCOMPARE(view.resolvedPaths(), QStringList{QStringLiteral("a.txt")});
    auto* list = view.findChild<QTreeWidget*>(QStringLiteral("resolvedFiles"));
    QVERIFY(list && !list->isHidden());
    QCOMPARE(list->topLevelItemCount(), 1);

    QVERIFY2(view.markConflicted(QStringLiteral("a.txt"), &error), qPrintable(error));
    QCOMPARE(gitOut(m_path, {"ls-files", "-u"}), stagesBefore);
    QCOMPARE(view.conflictCount(), 1);
    QCOMPARE(view.currentPath(), QStringLiteral("a.txt"));
    QVERIFY(view.resolvedPaths().isEmpty());
    QCOMPARE(readFile(m_path, "a.txt"), QStringLiteral("line1\nMAIN\nline3\n"));

    QVERIFY(!view.markConflicted(QStringLiteral("a.txt"), &error));
}

void ConflictViewTest::theResolvedListEndsWithTheOperation() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();
    QVERIFY(view.setRegionChoice(0, QStringLiteral("theirs")));
    QString error;
    QVERIFY2(view.trySaveResolution(&error), qPrintable(error));
    view.reload();
    QCOMPARE(view.resolvedPaths().size(), 1);

    QVERIFY(git(m_path, kIdentity + QStringList{"commit", "-q", "--no-edit"}));
    // git keeps the resolve-undo record past the commit; the list must not.
    QVERIFY(gitOut(m_path, {"ls-files", "--resolve-undo"}).contains(QStringLiteral("a.txt")));
    view.reload();
    QVERIFY(view.resolvedPaths().isEmpty());
}

void ConflictViewTest::aDraftIsSavedWithItsMarkersAndNothingStaged() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    ConflictView view(&session);
    view.reload();

    // Half-done work can be put on disk with its markers kept: the conflict stays
    // open and nothing is staged, so the reader can come back to it.
    QString error;
    QVERIFY2(view.saveDraft(&error), qPrintable(error));
    QVERIFY(readFile(m_path, "a.txt").contains(QStringLiteral("<<<<<<<")));
    QVERIFY(!session.conflicts().empty());
    QVERIFY(!gitOut(m_path, {"ls-files", "-u"}).trimmed().isEmpty());
}

QTEST_MAIN(ConflictViewTest)
#include "test_conflict_view.moc"
