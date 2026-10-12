#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/conflict_view.h"
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

QString readFile(const QString& dir, const QString& name) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

/// rerere integration, against real repositories: the point is that git applies a
/// recorded resolution by itself during the merge, and the window must notice the
/// file is resolved on disk while the index still calls it conflicted.
class RerereTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void togglingRerereIsRememberedInTheConfig();
    void aFreshConflictIsTracked();
    void aRecordedResolutionIsAppliedAndStaged();
    void theWindowReportsRerereState();

private:
    void buildConflict();
    /// Resolve, record, abort, and merge again so rerere applies from memory.
    void recordThenReplay();

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void RerereTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    buildConflict();
}

void RerereTest::cleanup() {
    m_dir.reset();
}

void RerereTest::buildConflict() {
    QVERIFY(writeFile(m_path, "a.txt", "line1\nline2\nline3\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "base"}));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nFEATURE\nline3\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "feature"}));

    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nMAIN\nline3\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "main"}));
}

void RerereTest::recordThenReplay() {
    // First meeting: conflict, resolve by hand, let rerere record it, rewind.
    QVERIFY(!git(m_path, identity() + QStringList{"merge", "feature"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nRESOLVED\nline3\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, {"rerere"}));
    git(m_path, {"merge", "--abort"});

    // Second meeting: the same conflict, resolved from memory.
    QVERIFY(!git(m_path, identity() + QStringList{"merge", "feature"}));
}

void RerereTest::togglingRerereIsRememberedInTheConfig() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    QVERIFY2(session.setRerereEnabled(true, &error), qPrintable(error));
    QVERIFY(session.rerereEnabled());
    QCOMPARE(gitOut(m_path, {"config", "--bool", "--get", "rerere.enabled"}).trimmed(),
             QStringLiteral("true"));

    QVERIFY2(session.setRerereAutoupdate(true, &error), qPrintable(error));
    QVERIFY(session.rerereAutoupdate());

    QVERIFY2(session.setRerereEnabled(false, &error), qPrintable(error));
    QVERIFY(!session.rerereEnabled());
}

void RerereTest::aFreshConflictIsTracked() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY(session.setRerereEnabled(true, &error));

    // A conflict with nothing recorded yet: rerere tracks it, and has no memory
    // to apply, so it is still "remaining".
    QVERIFY(!git(m_path, identity() + QStringList{"merge", "feature"}));
    QVERIFY(session.rerereTrackedPaths().contains(QStringLiteral("a.txt")));
    QVERIFY(session.rerereRemainingPaths().contains(QStringLiteral("a.txt")));

    ConflictView view(&session);
    view.reload();
    QVERIFY(view.rerereRemembered());
    QVERIFY(!view.rerereApplied());

    QVERIFY2(session.forgetRerere(QStringLiteral("a.txt"), &error), qPrintable(error));
}

void RerereTest::aRecordedResolutionIsAppliedAndStaged() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY(session.setRerereEnabled(true, &error));

    recordThenReplay();

    // The merge stopped, but rerere put the resolution in the working tree: the
    // index still carries the conflict while the file on disk is resolved.
    QVERIFY(!session.conflicts().empty());
    QCOMPARE(readFile(m_path, "a.txt"), QStringLiteral("line1\nRESOLVED\nline3\n"));
    QVERIFY(session.rerereRemainingPaths().isEmpty());

    ConflictView view(&session);
    view.reload();
    QVERIFY(view.rerereApplied());
    QVERIFY2(view.stageRecordedResolution(&error), qPrintable(error));
    QVERIFY(session.conflicts().empty());
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).trimmed().isEmpty());
}

void RerereTest::theWindowReportsRerereState() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QStringList enabled =
        window.rerereForDiagnostics(QStringLiteral("enable"), QString());
    QVERIFY2(enabled.join(QLatin1Char('\n')).contains(QStringLiteral("enabled: yes")),
             qPrintable(enabled.join(QLatin1Char('\n'))));
    QVERIFY(!git(m_path, identity() + QStringList{"merge", "feature"}));

    const QStringList status =
        window.rerereForDiagnostics(QStringLiteral("status"), QString());
    const QString text = status.join(QLatin1Char('\n'));
    QVERIFY2(text.contains(QStringLiteral("tracked: a.txt")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("remaining: a.txt")));
}

QTEST_MAIN(RerereTest)
#include "test_rerere.moc"
