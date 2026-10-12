#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/main_window.h"

#include <QFile>
#include <QSet>
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

QString gitOut(const QString& workDir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const auto& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, workDir.toStdString()).out).trimmed();
}

QStringList linesOf(const QString& text) {
    QStringList out;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (!line.isEmpty()) {
            out << line;
        }
    }
    return out;
}

} // namespace

class CommitSearchTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

private slots:
    void init();
    void cleanup();

    void messageSearchMatchesTheBinary();
    void authorSearchMatchesSubstrings();
    void aShaResolvesToOneCommit();
    void pagingSkipsAndLimits();
    void theWindowSearchesAndReveals();
};

void CommitSearchTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));

    const auto commitAs = [this](const QString& name, const QString& email, const QString& message,
                                 const QString& file) {
        QFile f(m_path + QLatin1Char('/') + file);
        if (!f.open(QIODevice::WriteOnly)) {
            return false;
        }
        f.write(message.toUtf8());
        f.close();
        return git(m_path, {"add", "-A"}) &&
               git(m_path, {"-c", "user.name=" + name, "-c", "user.email=" + email, "commit", "-q",
                            "-m", message});
    };

    QVERIFY(commitAs(QStringLiteral("Alice"), QStringLiteral("alice@example.com"),
                     QStringLiteral("feat: add login"), QStringLiteral("a.txt")));
    QVERIFY(commitAs(QStringLiteral("Bob"), QStringLiteral("bob@example.com"),
                     QStringLiteral("fix: correct logout"), QStringLiteral("b.txt")));
    QVERIFY(commitAs(QStringLiteral("Alice"), QStringLiteral("alice@example.com"),
                     QStringLiteral("docs: readme"), QStringLiteral("c.txt")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitAs(QStringLiteral("Carol"), QStringLiteral("carol@example.com"),
                     QStringLiteral("fix: branch bug"), QStringLiteral("d.txt")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
}

void CommitSearchTest::cleanup() {
    m_dir.reset();
}

void CommitSearchTest::messageSearchMatchesTheBinary() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const std::vector<CommitInfo> results =
        session.searchCommitsBlocking(QStringLiteral("FIX"), CommitSearchField::Message, 50);
    QCOMPARE(results.size(), std::size_t(2));
    for (const CommitInfo& commit : results) {
        QVERIFY(QString::fromStdString(commit.summary).contains(QStringLiteral("fix")));
    }

    // Oracle: the same query through the binary.
    const QStringList expected =
        linesOf(gitOut(m_path, {"log", "--all", "-i", "-F", "--grep=fix", "--format=%H"}));
    QCOMPARE(results.size(), static_cast<std::size_t>(expected.size()));
    QSet<QString> got;
    for (const CommitInfo& commit : results) {
        got.insert(QString::fromStdString(commit.id));
    }
    QCOMPARE(got, QSet<QString>(expected.begin(), expected.end()));
}

void CommitSearchTest::authorSearchMatchesSubstrings() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const std::vector<CommitInfo> results =
        session.searchCommitsBlocking(QStringLiteral("carol"), CommitSearchField::Author, 50);
    QCOMPARE(results.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(results.front().author.name), QStringLiteral("Carol"));
}

void CommitSearchTest::aShaResolvesToOneCommit() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const QString head = gitOut(m_path, {"rev-parse", "HEAD"});
    const std::vector<CommitInfo> resolved =
        session.searchCommitsBlocking(head, CommitSearchField::Sha, 50);
    QCOMPARE(resolved.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(resolved.front().id), head);

    const std::vector<CommitInfo> abbreviated =
        session.searchCommitsBlocking(head.left(8), CommitSearchField::Sha, 50);
    QCOMPARE(abbreviated.size(), std::size_t(1));

    // Something that names no commit is an empty answer, not an error.
    QVERIFY(session.searchCommitsBlocking(QStringLiteral("deadbeefdeadbeef"),
                                          CommitSearchField::Sha, 50)
                .empty());
}

void CommitSearchTest::pagingSkipsAndLimits() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const std::vector<CommitInfo> all =
        session.searchCommitsBlocking(QStringLiteral("fix"), CommitSearchField::Message, 50);
    QCOMPARE(all.size(), std::size_t(2));

    const std::vector<CommitInfo> first =
        session.searchCommitsBlocking(QStringLiteral("fix"), CommitSearchField::Message, 1, 0);
    const std::vector<CommitInfo> second =
        session.searchCommitsBlocking(QStringLiteral("fix"), CommitSearchField::Message, 1, 1);
    QCOMPARE(first.size(), std::size_t(1));
    QCOMPARE(second.size(), std::size_t(1));
    QVERIFY(first.front().id != second.front().id);
    QCOMPARE(QString::fromStdString(first.front().id), QString::fromStdString(all.at(0).id));
    QCOMPARE(QString::fromStdString(second.front().id), QString::fromStdString(all.at(1).id));
}

void CommitSearchTest::theWindowSearchesAndReveals() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QStringList report =
        window.searchForDiagnostics(QStringLiteral("fix"), QStringLiteral("grep"));
    QVERIFY2(report.value(0) == QStringLiteral("results: 2"), qPrintable(report.join('\n')));

    const QString head = gitOut(m_path, {"rev-parse", "HEAD"});
    const QStringList revealed =
        window.searchForDiagnostics(QStringLiteral("fix"), QStringLiteral("grep"), head);
    QVERIFY2(revealed.join(QLatin1Char('\n')).contains(QStringLiteral("revealed row ")),
             qPrintable(revealed.join(QLatin1Char('\n'))));

    // A commit the graph does not have is reported, not silently ignored.
    const QStringList missing = window.searchForDiagnostics(
        QStringLiteral("fix"), QStringLiteral("grep"), QStringLiteral("deadbeefdeadbeef"));
    QVERIFY2(missing.join(QLatin1Char('\n')).contains(QStringLiteral("not found")),
             qPrintable(missing.join(QLatin1Char('\n'))));
}

QTEST_MAIN(CommitSearchTest)
#include "test_commit_search.moc"
