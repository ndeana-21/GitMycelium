#include "core/types.h"
#include "git/cli.h"
#include "git/repository.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

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

bool writeFile(const QString& path, const QString& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}

QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

bool listedInStatus(const std::vector<FileChange>& changes, const QString& path) {
    return std::any_of(changes.begin(), changes.end(), [&](const FileChange& change) {
        return QString::fromStdString(change.path) == path;
    });
}

int occurrences(const std::string& text, const std::string& needle) {
    int count = 0;
    for (std::size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

} // namespace

class CommitHygieneTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

private slots:
    void initTestCase();
    void init();

    void aFreshPatternIsWrittenOnce();
    void theExcludeFileIsUsedForTheGlobalScope();
    void anUntrackedFileLeavesTheStatus();
    void untrackingKeepsTheFileOnDisk();
    void untrackingAPathspecThatMatchesNothingIsFine();
    void coauthorsLandInATrailerBlock();
    void aCoauthorAlreadyThereIsNotAddedTwice();
    void theCommitTemplateIsResolved();
    void recentAuthorsAreNewestFirstAndDeduped();
};

void CommitHygieneTest::initTestCase() {
    Repository::initLibrary();
}

void CommitHygieneTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Demo Dev"}));
    QVERIFY(git(m_path, {"config", "user.email", "demo@example.com"}));
    QVERIFY(git(m_path, {"config", "commit.gpgsign", "false"}));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("a.txt")), QStringLiteral("a\n")));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
}

void CommitHygieneTest::aFreshPatternIsWrittenOnce() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    repo->addIgnorePattern("*.log", false);
    repo->addIgnorePattern("*.log", false);

    const QString ignore = readFile(QDir(m_path).filePath(QStringLiteral(".gitignore")));
    const QStringList lines = ignore.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.count(QStringLiteral("*.log")), 1);
}

void CommitHygieneTest::theExcludeFileIsUsedForTheGlobalScope() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    repo->addIgnorePattern("secret.txt", true);

    const QString exclude =
        readFile(QDir(m_path).filePath(QStringLiteral(".git/info/exclude")));
    QVERIFY2(exclude.contains(QStringLiteral("secret.txt")), qPrintable(exclude));
}

void CommitHygieneTest::anUntrackedFileLeavesTheStatus() {
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("x.log")), QStringLiteral("noise\n")));
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    QVERIFY(listedInStatus(repo->status(), QStringLiteral("x.log")));

    repo->addIgnorePattern("/x.log", false);
    QVERIFY(!listedInStatus(repo->status(), QStringLiteral("x.log")));
}

void CommitHygieneTest::untrackingKeepsTheFileOnDisk() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    repo->untrackPathspec(":(literal)a.txt");

    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("a.txt"))));
    QVERIFY(gitOut(m_path, {"ls-files"}).isEmpty());
    // Untracked, not deleted: the status lists it again, which is what makes the
    // following ignore pattern the thing that hides it.
    QVERIFY(listedInStatus(repo->status(), QStringLiteral("a.txt")));
}

void CommitHygieneTest::untrackingAPathspecThatMatchesNothingIsFine() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    // Ignoring a file git has never tracked is the ordinary case; `git rm` would
    // call it a fatal "did not match", and the layer must not.
    repo->untrackPathspec(":(literal)never-tracked.txt");
    QVERIFY(true);
}

void CommitHygieneTest::coauthorsLandInATrailerBlock() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    const std::string message =
        repo->addCoAuthors("Subject line\n", {CoAuthor{"Ada Lovelace", "ada@example.com"}});
    QVERIFY2(message.find("Co-authored-by: Ada Lovelace <ada@example.com>") != std::string::npos,
             message.c_str());
    QVERIFY(message.find("Subject line") != std::string::npos);
}

void CommitHygieneTest::aCoauthorAlreadyThereIsNotAddedTwice() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    const CoAuthor ada{"Ada Lovelace", "ada@example.com"};
    const std::string once = repo->addCoAuthors("Subject\n", {ada});
    const std::string twice = repo->addCoAuthors(once, {ada});
    QCOMPARE(occurrences(twice, "Co-authored-by: Ada Lovelace"), 1);
}

void CommitHygieneTest::theCommitTemplateIsResolved() {
    const QString templatePath = QDir(m_path).filePath(QStringLiteral("message.txt"));
    QVERIFY(writeFile(templatePath, QStringLiteral("Ticket: \n")));
    QVERIFY(git(m_path, {"config", "commit.template", templatePath}));

    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    QCOMPARE(QString::fromStdString(repo->commitTemplate()), QStringLiteral("Ticket: \n"));
}

void CommitHygieneTest::recentAuthorsAreNewestFirstAndDeduped() {
    QVERIFY(git(m_path, {"commit", "--allow-empty", "--author", "Other Person <other@example.com>",
                         "-qm", "second"}));
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    const std::vector<CoAuthor> authors = repo->recentAuthors(20);
    QCOMPARE(authors.size(), std::size_t(2));
    QCOMPARE(QString::fromStdString(authors.front().email), QStringLiteral("other@example.com"));
    QCOMPARE(QString::fromStdString(authors.back().email), QStringLiteral("demo@example.com"));
}

QTEST_MAIN(CommitHygieneTest)
#include "test_commit_hygiene.moc"
