#include "core/types.h"
#include "git/cli.h"
#include "git/repository.h"
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

std::vector<CommitHit> search(const QString& workDir, const CommitQuery& query,
                              std::size_t limit = 50) {
    std::vector<CommitHit> found;
    const std::string error = Repository::streamCommitSearch(
        workDir.toStdString(), query, limit, [&found](std::vector<CommitHit>&& batch) {
            for (CommitHit& hit : batch) {
                found.push_back(std::move(hit));
            }
            return true;
        });
    if (!error.empty()) {
        qWarning() << "search failed:" << QString::fromStdString(error);
    }
    return found;
}

CommitQuery byText(const QString& text) {
    CommitQuery query;
    query.text = text.toStdString();
    return query;
}

QSet<QString> idsOf(const std::vector<CommitHit>& hits) {
    QSet<QString> ids;
    for (const CommitHit& hit : hits) {
        ids.insert(QString::fromStdString(hit.commit.id));
    }
    return ids;
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
    void aShaComesFirst();
    void wildcardsMatchInsideTheText();
    void aDateRangeKeepsItsCommits();
    void theSourceNamesTheRefAndTheBodyIsKept();
    void aSearchStopsWhenAsked();
    void theLimitCapsTheResults();
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
    const std::vector<CommitHit> results = search(m_path, byText(QStringLiteral("FIX")));
    QCOMPARE(results.size(), std::size_t(2));
    for (const CommitHit& hit : results) {
        QVERIFY(QString::fromStdString(hit.commit.summary).contains(QStringLiteral("fix")));
    }

    // Oracle: the same query through the binary.
    const QStringList expected =
        linesOf(gitOut(m_path, {"log", "--all", "-i", "-F", "--grep=fix", "--format=%H"}));
    QCOMPARE(idsOf(results), QSet<QString>(expected.begin(), expected.end()));
}

void CommitSearchTest::authorSearchMatchesSubstrings() {
    // One text, both fields: Carol is an author, "login" is a message.
    std::vector<CommitHit> results = search(m_path, byText(QStringLiteral("carol")));
    QCOMPARE(results.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(results.front().commit.author.name), QStringLiteral("Carol"));
    QCOMPARE(search(m_path, byText(QStringLiteral("EXAMPLE.COM"))).size(), std::size_t(4));

    // A commit matching both is listed once.
    QVERIFY(git(m_path, {"-c", "user.name=Login Larry", "-c", "user.email=l@example.org",
                         "commit", "-q", "--allow-empty", "-m", "login again"}));
    results = search(m_path, byText(QStringLiteral("login")));
    QCOMPARE(results.size(), std::size_t(2));
    QCOMPARE(idsOf(results).size(), 2);
    QCOMPARE(search(m_path, byText(QStringLiteral("l*larry"))).size(), std::size_t(1));
}

void CommitSearchTest::aShaComesFirst() {
    const QString head = gitOut(m_path, {"rev-parse", "HEAD"});
    std::vector<CommitHit> resolved = search(m_path, byText(head));
    QCOMPARE(resolved.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(resolved.front().commit.id), head);

    resolved = search(m_path, byText(head.left(8)));
    QVERIFY(!resolved.empty());
    QCOMPARE(QString::fromStdString(resolved.front().commit.id), head);

    QVERIFY(search(m_path, byText(QStringLiteral("deadbeefdeadbeef"))).empty());
}

void CommitSearchTest::wildcardsMatchInsideTheText() {
    // "*text*" is the plain search; a star inside the query spans anything.
    std::vector<CommitHit> results = search(m_path, byText(QStringLiteral("f*bug")));
    QCOMPARE(results.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(results.front().commit.summary),
             QStringLiteral("fix: branch bug"));

    QCOMPARE(search(m_path, byText(QStringLiteral("log?n"))).size(), std::size_t(1));
    // Regex characters in a wildcard query are literal.
    QVERIFY(search(m_path, byText(QStringLiteral("fix.*bug"))).empty());
    QVERIFY(search(m_path, byText(QStringLiteral("(fix*"))).empty());
}

void CommitSearchTest::aDateRangeKeepsItsCommits() {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    CommitQuery query;
    query.since = now - 3600;
    query.until = now + 3600;
    QCOMPARE(search(m_path, query).size(), std::size_t(4));

    query.text = "fix";
    QCOMPARE(search(m_path, query).size(), std::size_t(2));

    query.text.clear();
    query.since = 1;
    query.until = 1000;
    QVERIFY(search(m_path, query).empty());
}

void CommitSearchTest::theSourceNamesTheRefAndTheBodyIsKept() {
    const std::vector<CommitHit> results = search(m_path, byText(QStringLiteral("branch bug")));
    QCOMPARE(results.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(results.front().source), QStringLiteral("refs/heads/feature"));
    QCOMPARE(QString::fromStdString(results.front().commit.author.email),
             QStringLiteral("carol@example.com"));

    QVERIFY(git(m_path, {"-c", "user.name=Dan", "-c", "user.email=dan@example.com", "commit",
                         "-q", "--allow-empty", "-m", "subject line", "-m", "body text\nsecond"}));
    const std::vector<CommitHit> withBody = search(m_path, byText(QStringLiteral("subject line")));
    QCOMPARE(withBody.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(withBody.front().commit.summary), QStringLiteral("subject line"));
    QCOMPARE(QString::fromStdString(withBody.front().commit.body),
             QStringLiteral("body text\nsecond"));
}

void CommitSearchTest::aSearchStopsWhenAsked() {
    for (int i = 0; i < 300; ++i) {
        QVERIFY(git(m_path, {"-c", "user.name=Eve", "-c", "user.email=eve@example.com", "commit",
                             "-q", "--allow-empty", "-m", QStringLiteral("bulk %1").arg(i)}));
    }
    int batches = 0;
    std::size_t delivered = 0;
    const std::string error = Repository::streamCommitSearch(
        m_path.toStdString(), byText(QStringLiteral("bulk")), 1000,
        [&](std::vector<CommitHit>&& batch) {
            ++batches;
            delivered += batch.size();
            return false;
        });
    QVERIFY(error.empty());
    QCOMPARE(batches, 1);
    QVERIFY(delivered < 300);
}

void CommitSearchTest::theLimitCapsTheResults() {
    QCOMPARE(search(m_path, byText(QStringLiteral(":")), 3).size(), std::size_t(3));
}

void CommitSearchTest::theWindowSearchesAndReveals() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    QStringList report = window.searchForDiagnostics(QStringLiteral("fix"), QStringLiteral("text"));
    QString text = report.join(QLatin1Char('\n'));
    QVERIFY2(text.contains(QStringLiteral("== Commits · 2")), qPrintable(text));
    QCOMPARE(report.filter(QStringLiteral("commit ")).size(), 2);
    // Ctrl+F lands on history, not on a command that happens to match.
    QVERIFY2(report.filter(QStringLiteral("> commit ")).size() == 1, qPrintable(text));

    // Branches before commits, each block under its own heading.
    report = window.searchForDiagnostics(QStringLiteral("fea"), QStringLiteral("text"));
    text = report.join(QLatin1Char('\n'));
    const int branches = report.indexOf(QStringLiteral("== Branches · 1"));
    const int commits = report.indexOf(QRegularExpression(QStringLiteral("== Commits.*")));
    QVERIFY2(branches >= 0 && commits > branches, qPrintable(text));
    QVERIFY(report.contains(QStringLiteral("> ref feature")));
    QVERIFY(text.contains(QStringLiteral("[feature] fix: branch bug")) ||
            text.contains(QStringLiteral("feat: add login")));

    report = window.searchForDiagnostics(QStringLiteral("alice"), QStringLiteral("text"));
    QCOMPARE(report.filter(QStringLiteral("commit ")).size(), 2);
    QVERIFY(report.filter(QStringLiteral("== Branches")).isEmpty());
    // Newest first, whichever search found the row.
    const QStringList rows = report.filter(QStringLiteral("commit "));
    QVERIFY2(rows.at(0).contains(QStringLiteral("docs: readme")), qPrintable(rows.join('\n')));

    const QString today = QDate::currentDate().toString(Qt::ISODate);
    report = window.searchForDiagnostics(QStringLiteral("fix"),
                                         QStringLiteral("dates:%1..%1").arg(today));
    QCOMPARE(report.filter(QStringLiteral("commit ")).size(), 2);
    report = window.searchForDiagnostics(QString(), QStringLiteral("dates:2001-01-01..2001-12-31"));
    QVERIFY2(report.contains(QStringLiteral("== Commits · nothing found")),
             qPrintable(report.join(QLatin1Char('\n'))));

    const QString head = gitOut(m_path, {"rev-parse", "HEAD"});
    const QStringList revealed =
        window.searchForDiagnostics(QStringLiteral("fix"), QStringLiteral("text"), head);
    QVERIFY2(revealed.join(QLatin1Char('\n')).contains(QStringLiteral("revealed row ")),
             qPrintable(revealed.join(QLatin1Char('\n'))));

    const QStringList missing = window.searchForDiagnostics(
        QStringLiteral("fix"), QStringLiteral("text"), QStringLiteral("deadbeefdeadbeef"));
    QVERIFY2(missing.join(QLatin1Char('\n')).contains(QStringLiteral("not found")),
             qPrintable(missing.join(QLatin1Char('\n'))));
}

QTEST_MAIN(CommitSearchTest)
#include "test_commit_search.moc"
