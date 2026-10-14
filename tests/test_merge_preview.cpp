#include "core/conflict_regions.h"
#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/graph_operation_panel.h"
#include "ui/main_window.h"

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
    return QString::fromStdString(runGit(argv, dir.toStdString()).out);
}

QStringList identity() { return {QStringLiteral("-c"), QStringLiteral("user.name=Test"),
                                 QStringLiteral("-c"), QStringLiteral("user.email=test@localhost")}; }

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

} // namespace

class MergePreviewTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aConflictingPairNamesItsFiles();
    void aCleanPairIsClean();
    void theDropLineNamesWhatItKnows();
    void theWindowReportsAPreview();
    void thePredictionPointsAtTheLines();

private:
    void buildRepository();

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void MergePreviewTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    buildRepository();
}

void MergePreviewTest::cleanup() {
    m_dir.reset();
}

void MergePreviewTest::buildRepository() {
    QVERIFY(writeFile(m_path, "a.txt", "line1\nline2\nline3\n"));
    QVERIFY(writeFile(m_path, "b.txt", "b\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "base"}));

    // feature touches the same line main will touch: a real conflict.
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nFEATURE\nline3\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "feature"}));

    // cleanbranch adds an unrelated file: a merge with main is clean.
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "cleanbranch", "main"}));
    QVERIFY(writeFile(m_path, "c.txt", "c\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "clean branch"}));

    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nMAIN\nline3\n"));
    QVERIFY(writeFile(m_path, "b.txt", "b2\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "main"}));
}

void MergePreviewTest::aConflictingPairNamesItsFiles() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    const MergePreview preview = session.previewMerge(QStringLiteral("main"),
                                                      QStringLiteral("feature"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(preview.available);
    QVERIFY(!preview.clean);
    QCOMPARE(preview.conflictedPaths.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(preview.conflictedPaths.at(0)), QStringLiteral("a.txt"));

    QVERIFY(!session.mergeInProgress());
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).trimmed().isEmpty());
}

void MergePreviewTest::aCleanPairIsClean() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    const MergePreview preview = session.previewMerge(QStringLiteral("main"),
                                                      QStringLiteral("cleanbranch"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(preview.available);
    QVERIFY(preview.clean);
    QVERIFY(preview.conflictedPaths.empty());
}

void MergePreviewTest::theDropLineNamesWhatItKnows() {
    // The line the drop list shows: it must always say something — a reader cannot tell
    // "no conflicts" from "the check failed" if both are blank.
    QCOMPARE(describeMergePreview(MergePreview{}),
             QStringLiteral("Nothing to predict — already up to date"));

    MergePreview clean;
    clean.available = true;
    clean.clean = true;
    QCOMPARE(describeMergePreview(clean), QStringLiteral("No conflicts predicted"));

    MergePreview conflict;
    conflict.available = true;
    conflict.clean = false;
    conflict.conflictedPaths = {"a.txt", "b.txt"};
    const QString text = describeMergePreview(conflict);
    QVERIFY(text.contains(QStringLiteral("Conflicts in 2 file(s)")));
    QVERIFY(text.contains(QStringLiteral("a.txt")));
    QVERIFY(text.contains(QStringLiteral("b.txt")));
}

void MergePreviewTest::theWindowReportsAPreview() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QStringList report = window.mergePreviewForDiagnostics(QStringLiteral("main"),
                                                                 QStringLiteral("feature"));
    const QString text = report.join(QLatin1Char('\n'));
    QVERIFY2(text.contains(QStringLiteral("clean: no")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("conflicts: 1")));
    QVERIFY(text.contains(QStringLiteral("path: a.txt")));
}

void MergePreviewTest::thePredictionPointsAtTheLines() {
    // Twenty lines; the sides disagree on line 3, on lines 10–11, and on line 20,
    // which ours deleted while the other side changed it — a conflict with no lines
    // of ours in it, a point before what would follow. The three are far enough
    // apart that git does not fold them into one region (closer ones it does).
    const auto lines = [](const QHash<int, QString>& changed, int drop) {
        QString text;
        for (int i = 1; i <= 20; ++i) {
            if (i != drop) {
                text += changed.value(i, QStringLiteral("l%1").arg(i)) + QLatin1Char('\n');
            }
        }
        return text;
    };
    QVERIFY(writeFile(m_path, "n.txt", lines({}, 0)));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "twenty"}));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "far", "main"}));
    QVERIFY(writeFile(m_path, "n.txt",
                      lines({{3, QStringLiteral("FAR3")}, {10, QStringLiteral("FAR10")},
                             {11, QStringLiteral("FAR11")}, {20, QStringLiteral("FAR20")}},
                            0)));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-am", "far"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path, "n.txt",
                      lines({{3, QStringLiteral("NEAR3")}, {10, QStringLiteral("NEAR10")},
                             {11, QStringLiteral("NEAR11")}},
                            20)));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-am", "near"}));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    const MergePreview preview =
        session.previewMerge(QStringLiteral("main"), QStringLiteral("far"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto it = preview.conflictedLines.find("n.txt");
    QVERIFY(it != preview.conflictedLines.end());
    const std::vector<std::pair<std::size_t, std::size_t>> expected = {{3, 1}, {10, 2}, {20, 0}};
    std::vector<std::pair<std::size_t, std::size_t>> actual;
    for (const LineSpan& span : it->second) {
        actual.emplace_back(span.first, span.count);
    }
    QVERIFY(actual == expected);

    // The same places the real merge marks: run it and read the markers back.
    QVERIFY(!git(m_path, identity() + QStringList{"merge", "-q", "far"}));
    QFile merged(QDir(m_path).filePath(QStringLiteral("n.txt")));
    QVERIFY(merged.open(QIODevice::ReadOnly));
    std::vector<std::pair<std::size_t, std::size_t>> fromMerge;
    for (const LineSpan& span : oursLineSpans(parseConflictDocument(merged.readAll().toStdString()))) {
        fromMerge.emplace_back(span.first, span.count);
    }
    QVERIFY(fromMerge == expected);
}

QTEST_MAIN(MergePreviewTest)
#include "test_merge_preview.moc"
