/// The unified-diff parser, checked against libgit2 on a full repository: for every
/// changed path, what git prints must parse into the same FileDiff libgit2 builds.
/// This is the oracle that lets the parser stand in for libgit2 on a promisor clone.

#include "core/types.h"
#include "core/unified_diff.h"
#include "git/cli.h"
#include "repo/repo_session.h"

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

/// The raw diff text, with no trimming: the parser is what is under test.
std::string gitRaw(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).out;
}

bool writeFile(const QString& dir, const QString& name, const std::string& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QByteArray::fromStdString(content));
    return true;
}

/// Every field the app reads, compared one by one so a failure names the field.
void expectSameFileDiff(const FileDiff& lib, const FileDiff& parsed, const QString& label) {
    QVERIFY2(lib.kind == parsed.kind, qPrintable(label + QStringLiteral(": kind")));
    QCOMPARE(QString::fromStdString(parsed.oldPath), QString::fromStdString(lib.oldPath));
    QCOMPARE(QString::fromStdString(parsed.newPath), QString::fromStdString(lib.newPath));
    QCOMPARE(parsed.binary, lib.binary);
    QCOMPARE(parsed.added, lib.added);
    QCOMPARE(parsed.removed, lib.removed);
    QCOMPARE(parsed.hunks.size(), lib.hunks.size());
    for (std::size_t h = 0; h < lib.hunks.size(); ++h) {
        const DiffHunk& a = lib.hunks[h];
        const DiffHunk& b = parsed.hunks[h];
        QCOMPARE(b.oldStart, a.oldStart);
        QCOMPARE(b.oldLines, a.oldLines);
        QCOMPARE(b.newStart, a.newStart);
        QCOMPARE(b.newLines, a.newLines);
        QCOMPARE(b.lines.size(), a.lines.size());
        for (std::size_t i = 0; i < a.lines.size(); ++i) {
            const DiffLine& la = a.lines[i];
            const DiffLine& lb = b.lines[i];
            QVERIFY2(la.kind == lb.kind, qPrintable(label + QStringLiteral(": line kind")));
            QCOMPARE(lb.oldLine, la.oldLine);
            QCOMPARE(lb.newLine, la.newLine);
            QVERIFY2(la.text == lb.text, qPrintable(label + QStringLiteral(": line text")));
            QCOMPARE(lb.noNewline, la.noNewline);
        }
    }
}

} // namespace

class UnifiedDiffTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void everyChangedPathMatchesLibgit2();
    void anEmptyDiffHasNoEntries();
    void multipleFilesInOneDiffParseSeparately();

private:
    QString diffTextFor(const QString& path) const;
    bool commit(const QString& message);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    QString m_c1;
    QString m_c2;
};

void UnifiedDiffTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));

    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), "one\ntwo\nthree\n"));
    QVERIFY(writeFile(m_path, QStringLiteral("gone.txt"), "bye\n"));
    std::string multi;
    for (int i = 1; i <= 20; ++i) {
        multi += "line " + std::to_string(i) + "\n";
    }
    QVERIFY(writeFile(m_path, QStringLiteral("multi.txt"), multi));
    QVERIFY(commit(QStringLiteral("c1")));
    m_c1 = gitOut(m_path, {"rev-parse", "HEAD"});

    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), "one\nTWO\nthree\n"));
    QVERIFY(writeFile(m_path, QStringLiteral("new.txt"), "n1\nn2\n"));
    QVERIFY(git(m_path, {"rm", "-q", "gone.txt"}));
    std::string multiChanged = multi;
    // Change line 2 and line 18, far enough apart to be two hunks.
    multiChanged.replace(multiChanged.find("line 2\n"), 7, "LINE TWO\n");
    multiChanged.replace(multiChanged.find("line 18\n"), 8, "LINE EIGHTEEN\n");
    QVERIFY(writeFile(m_path, QStringLiteral("multi.txt"), multiChanged));
    QVERIFY(writeFile(m_path, QStringLiteral("nonl.txt"), "no newline at end"));
    std::string binary;
    binary += "B";
    binary += '\0';
    binary += "X";
    QVERIFY(writeFile(m_path, QStringLiteral("bin.dat"), binary));
    QVERIFY(commit(QStringLiteral("c2")));
    m_c2 = gitOut(m_path, {"rev-parse", "HEAD"});
}

void UnifiedDiffTest::cleanup() {
    m_dir.reset();
}

bool UnifiedDiffTest::commit(const QString& message) {
    if (!git(m_path, {"add", "-A"})) {
        return false;
    }
    return git(m_path, {"-c", "user.name=Test", "-c", "user.email=test@localhost", "commit",
                        "-q", "-m", message});
}

QString UnifiedDiffTest::diffTextFor(const QString& path) const {
    return QString::fromStdString(
        gitRaw(m_path, {"-c", "core.quotepath=false", "diff", "--no-color", "--no-ext-diff",
                        "--no-renames", "--src-prefix=a/", "--dst-prefix=b/", m_c1, m_c2,
                        "--", path}));
}

void UnifiedDiffTest::everyChangedPathMatchesLibgit2() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const QStringList paths = {QStringLiteral("a.txt"), QStringLiteral("new.txt"),
                               QStringLiteral("gone.txt"), QStringLiteral("multi.txt"),
                               QStringLiteral("nonl.txt"), QStringLiteral("bin.dat")};
    for (const QString& path : paths) {
        const FileDiff lib = session.commitFileDiff(m_c2, path);
        const std::vector<FileDiff> parsed =
            parseUnifiedDiff(diffTextFor(path).toStdString());
        QVERIFY2(parsed.size() == 1, qPrintable(path + QStringLiteral(": one entry")));
        expectSameFileDiff(lib, parsed.front(), path);
    }
}

void UnifiedDiffTest::anEmptyDiffHasNoEntries() {
    QCOMPARE(parseUnifiedDiff(diffTextFor(QStringLiteral("a.txt")).toStdString()).size(),
             std::size_t(1));
    const std::string empty = gitRaw(m_path, {"diff", m_c1, m_c1, "--", "a.txt"});
    QVERIFY(empty.empty());
    QVERIFY(parseUnifiedDiff(empty).empty());
    QVERIFY(parseUnifiedDiff("").empty());
}

void UnifiedDiffTest::multipleFilesInOneDiffParseSeparately() {
    const std::string text = gitRaw(m_path, {"-c", "core.quotepath=false", "diff", "--no-color",
                                             "--no-renames", m_c1, m_c2});
    const std::vector<FileDiff> entries = parseUnifiedDiff(text);
    // The fixture changed exactly six paths.
    QCOMPARE(entries.size(), std::size_t(6));
    QStringList names;
    for (const FileDiff& entry : entries) {
        names << QString::fromStdString(entry.newPath.empty() ? entry.oldPath : entry.newPath);
    }
    names.sort();
    QStringList expected = {QStringLiteral("a.txt"), QStringLiteral("bin.dat"),
                            QStringLiteral("gone.txt"), QStringLiteral("multi.txt"),
                            QStringLiteral("new.txt"), QStringLiteral("nonl.txt")};
    expected.sort();
    QCOMPARE(names, expected);
}

QTEST_MAIN(UnifiedDiffTest)
#include "test_unified_diff.moc"
