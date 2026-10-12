#include "git/cli.h"
#include "model/commit_model.h"
#include "ui/commit_graph_view.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
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

bool writeFile(const QString& path, const QByteArray& content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    return f.write(content) == content.size();
}

QString field(const QStringList& report, const QString& key) {
    for (const QString& line : report) {
        if (line.startsWith(key + QStringLiteral(": "))) {
            return line.section(QStringLiteral(": "), 1);
        }
    }
    return QString();
}

QStringList separatorLabels(const QStringList& report) {
    QStringList labels;
    for (const QString& line : report) {
        if (line.startsWith(QStringLiteral("separator "))) {
            labels << line.section(QStringLiteral("label="), 1);
        }
    }
    return labels;
}

} // namespace

class TimelineTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void timelineIsOffByDefault();
    void dayGroupingInsertsOneSeparatorPerDay();
    void aSeparatorIsNotACommit();
    void theGroupingIsRememberedPerRepository();
    void theGraphIsUnaffectedBySeparators();

private:
    /// Three commits, each on its own day, oldest first in real time so the graph
    /// order is unambiguous.
    void buildDatedRepo();
    /// Waits for a page to arrive after a grouping switch, then reports.
    QStringList report(MainWindow& window, const QString& op);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    MainWindow* m_window = nullptr;
};

void TimelineTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    buildDatedRepo();
}

void TimelineTest::cleanup() {
    m_dir.reset();
}

void TimelineTest::buildDatedRepo() {
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    const QStringList dates{QStringLiteral("2024-01-05T10:00:00"),
                            QStringLiteral("2024-02-10T09:00:00"),
                            QStringLiteral("2024-06-01T12:00:00")};
    int i = 0;
    for (const QString& date : dates) {
        ++i;
        QVERIFY(writeFile(m_path + "/a.txt", QStringLiteral("line %1\n").arg(i).toUtf8()));
        QVERIFY(git(m_path, {"add", "a.txt"}));
        qputenv("GIT_AUTHOR_DATE", date.toUtf8());
        qputenv("GIT_COMMITTER_DATE", date.toUtf8());
        QVERIFY(git(m_path, {"commit", "-qm", QStringLiteral("commit %1").arg(i)}));
        qunsetenv("GIT_AUTHOR_DATE");
        qunsetenv("GIT_COMMITTER_DATE");
    }
}

QStringList TimelineTest::report(MainWindow& window, const QString& op) {
    if (op != QStringLiteral("show")) {
        // The switch restarts the history; the caller waits for the page with
        // QTRY_VERIFY(window.commitsAreLoaded()) before asking for the report.
        window.timelineForDiagnostics(op);
    }
    return window.timelineForDiagnostics(QStringLiteral("show"));
}

void TimelineTest::timelineIsOffByDefault() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    QTRY_VERIFY(window.commitsAreLoaded());
    const QStringList r = report(window, QStringLiteral("show"));
    QCOMPARE(field(r, QStringLiteral("period")), QStringLiteral("none"));
    QCOMPARE(field(r, QStringLiteral("separators")), QStringLiteral("0"));
}

void TimelineTest::dayGroupingInsertsOneSeparatorPerDay() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    QTRY_VERIFY(window.commitsAreLoaded());
    report(window, QStringLiteral("day"));
    QTRY_VERIFY(window.commitsAreLoaded());
    const QStringList r = report(window, QStringLiteral("show"));
    QCOMPARE(field(r, QStringLiteral("period")), QStringLiteral("day"));
    // Three commits, three different days: a separator opens each of them.
    QCOMPARE(field(r, QStringLiteral("separators")), QStringLiteral("3"));
    const QStringList labels = separatorLabels(r);
    QCOMPARE(labels.size(), 3);
    for (const QString& label : labels) {
        QVERIFY(!label.isEmpty());
    }
    // Rows are commits plus separators, not one or the other.
    QCOMPARE(field(r, QStringLiteral("rows")), QStringLiteral("6"));
}

void TimelineTest::aSeparatorIsNotACommit() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    QTRY_VERIFY(window.commitsAreLoaded());
    report(window, QStringLiteral("day"));
    QTRY_VERIFY(window.commitsAreLoaded());

    auto* view = window.findChild<CommitGraphView*>();
    QVERIFY(view);
    auto* model = qobject_cast<CommitModel*>(view->model());
    QVERIFY(model);

    int separators = 0;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (!model->isTimelineRow(row)) {
            continue;
        }
        ++separators;
        QVERIFY(model->index(row, CommitModel::Message).data(CommitModel::TimelineRole).toBool());
        // No commit id, so no batch operation can act on it, and not selectable,
        // so the keyboard steps over it.
        QVERIFY(model->index(row, CommitModel::Message)
                    .data(CommitModel::CommitIdRole)
                    .toString()
                    .isEmpty());
        QVERIFY(!(model->flags(model->index(row, CommitModel::Message)) & Qt::ItemIsSelectable));
        // It carries a label and nothing else.
        QVERIFY(!model->index(row, CommitModel::Message).data(Qt::DisplayRole).toString().isEmpty());
        QVERIFY(model->index(row, CommitModel::Author).data(Qt::DisplayRole).toString().isEmpty());
    }
    QCOMPARE(separators, 3);
}

void TimelineTest::theGroupingIsRememberedPerRepository() {
    {
        MainWindow window;
        QVERIFY(window.openRepository(m_path));
        QTRY_VERIFY(window.commitsAreLoaded());
        report(window, QStringLiteral("month"));
        QTRY_VERIFY(window.commitsAreLoaded());
    }
    MainWindow reopened;
    QVERIFY(reopened.openRepository(m_path));
    QTRY_VERIFY(reopened.commitsAreLoaded());
    QCOMPARE(field(report(reopened, QStringLiteral("show")), QStringLiteral("period")),
             QStringLiteral("month"));
}

void TimelineTest::theGraphIsUnaffectedBySeparators() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    QTRY_VERIFY(window.commitsAreLoaded());
    auto* view = window.findChild<CommitGraphView*>();
    QVERIFY(view);
    auto* model = qobject_cast<CommitModel*>(view->model());
    QVERIFY(model);
    const int lanesOff = model->laneCount();

    report(window, QStringLiteral("day"));
    QTRY_VERIFY(window.commitsAreLoaded());

    // A separator takes no lane, so the linear history is still one lane.
    QCOMPARE(model->laneCount(), lanesOff);
    QCOMPARE(lanesOff, 1);
}

QTEST_MAIN(TimelineTest)
#include "test_timeline.moc"
