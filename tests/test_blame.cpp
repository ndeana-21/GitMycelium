#include "core/types.h"
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

QStringList gitBlameIds(const QString& dir, const QString& path, const QString& revision) {
    QStringList args{"blame", "--line-porcelain"};
    if (!revision.isEmpty()) {
        args << revision;
    }
    args << "--" << path;
    const QString out = gitOut(dir, args);

    QStringList ids;
    const QStringList lines = out.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        // A group starts with "<sha> <orig> <final> [<n>]"; content lines start with a tab.
        if (line.isEmpty() || line.startsWith(QLatin1Char('\t'))) {
            continue;
        }
        const QStringList fields = line.split(QLatin1Char(' '));
        if (fields.size() >= 3 && fields.at(0).size() == 40 &&
            fields.at(1).toInt() > 0 && fields.at(2).toInt() > 0) {
            ids << fields.at(0);
        }
    }
    return ids;
}

} // namespace

class BlameTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void blameNamesTheCommitPerLine();
    void blameOfTheWorkingTreeMarksUncommittedLines();
    void fileHistoryFollowsARename();
    void fileHistoryIsNewestFirst();

private:
    bool commit(const QString& message);
    QString m_c1;

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void BlameTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(m_path, QStringLiteral("file.txt"),
                      QStringLiteral("alpha\nbravo\ncharlie\n")));
    QVERIFY(commit(QStringLiteral("c1")));
    m_c1 = gitOut(m_path, {"rev-parse", "HEAD"}).trimmed();
    QVERIFY(writeFile(m_path, QStringLiteral("file.txt"),
                      QStringLiteral("alpha\nBRAVO\ncharlie\n")));
    QVERIFY(commit(QStringLiteral("c2")));
}

void BlameTest::cleanup() {
    m_dir.reset();
}

bool BlameTest::commit(const QString& message) {
    if (!git(m_path, {"add", "-A"})) {
        return false;
    }
    return git(m_path, {"-c", "user.name=Test", "-c", "user.email=test@localhost", "commit",
                        "-q", "-m", message});
}

void BlameTest::blameNamesTheCommitPerLine() {
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const std::vector<BlameLine> lines =
        session.blame(QStringLiteral("file.txt"), QStringLiteral("HEAD"));
    QCOMPARE(lines.size(), std::size_t(3));
    QCOMPARE(lines[0].finalLine, 1);
    QCOMPARE(lines[1].finalLine, 2);
    QCOMPARE(lines[2].finalLine, 3);
    QCOMPARE(QString::fromStdString(lines[0].commit), m_c1.left(40));
    QVERIFY(QString::fromStdString(lines[1].commit) != m_c1);
    QCOMPARE(QString::fromStdString(lines[2].commit), m_c1.left(40));

    const QStringList expected = gitBlameIds(m_path, QStringLiteral("file.txt"),
                                             QStringLiteral("HEAD"));
    QCOMPARE(lines.size(), static_cast<std::size_t>(expected.size()));
    for (std::size_t i = 0; i < lines.size(); ++i) {
        QCOMPARE(QString::fromStdString(lines[i].commit), expected.at(static_cast<int>(i)));
    }
}

void BlameTest::blameOfTheWorkingTreeMarksUncommittedLines() {
    QVERIFY(writeFile(m_path, QStringLiteral("file.txt"),
                      QStringLiteral("alpha\nBRAVO\nCHARLIE\n")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const std::vector<BlameLine> lines = session.blame(QStringLiteral("file.txt"));

    QCOMPARE(lines.size(), std::size_t(3));
    QVERIFY(!lines[0].uncommitted);
    QVERIFY(!lines[1].uncommitted);
    QVERIFY(lines[2].uncommitted);
    QVERIFY(lines[2].commit.empty());
}

void BlameTest::fileHistoryFollowsARename() {
    QVERIFY(git(m_path, {"mv", "file.txt", "renamed.txt"}));
    QVERIFY(writeFile(m_path, QStringLiteral("renamed.txt"),
                      QStringLiteral("alpha\nBRAVO\ncharlie\ndelta\n")));
    QVERIFY(commit(QStringLiteral("c3 rename and extend")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const std::vector<FileHistoryEntry> history = session.fileHistory(QStringLiteral("renamed.txt"));

    const QStringList expected =
        gitOut(m_path, {"log", "--follow", "--format=%H", "--", "renamed.txt"})
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(history.size(), static_cast<std::size_t>(expected.size()));
    QCOMPARE(history.size(), std::size_t(3));
    QStringList ours;
    for (const FileHistoryEntry& entry : history) {
        ours << QString::fromStdString(entry.id);
    }
    QCOMPARE(ours, expected);
    QVERIFY(QString::fromStdString(history.front().summary).contains(QStringLiteral("rename")));
}

void BlameTest::fileHistoryIsNewestFirst() {
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const std::vector<FileHistoryEntry> history = session.fileHistory(QStringLiteral("file.txt"));
    QCOMPARE(history.size(), std::size_t(2));
    QCOMPARE(QString::fromStdString(history.front().id), gitOut(m_path, {"rev-parse", "HEAD"}).trimmed());
    QCOMPARE(QString::fromStdString(history.back().id), m_c1.left(40));
}

QTEST_MAIN(BlameTest)
#include "test_blame.moc"
