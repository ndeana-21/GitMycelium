
#include "core/porcelain_status.h"
#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

#include <string>
#include <tuple>
#include <vector>

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

bool writeFile(const QString& dir, const QString& name, const QByteArray& content) {
    QFile file(QDir(dir).filePath(name));
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content);
    return true;
}

using Entry = std::tuple<std::string, ChangeKind, bool>;

std::vector<Entry> entries(const std::vector<FileChange>& changes) {
    std::vector<Entry> out;
    for (const FileChange& change : changes) {
        out.emplace_back(change.path, change.kind, change.staged);
    }
    return out;
}

} // namespace

class StatusTest : public QObject {
    Q_OBJECT

private slots:
    void theParserReadsEveryRecordKind();
    void everyKindOfChangeAtOnce();
};

void StatusTest::theParserReadsEveryRecordKind() {
    const std::string zero = std::string(40, '0');
    std::string text;
    text += "1 MM N... 100644 100644 100644 " + zero + " " + zero + " a file" + '\0';
    text += "2 R. N... 100644 100644 100644 " + zero + " " + zero + " R100 new name" + '\0' +
            "old name" + '\0';
    text += "u UU N... 100644 100644 100644 100644 " + zero + " " + zero + " " + zero + " c" +
            '\0';
    text += "1 .D N... 100644 100644 000000 " + zero + " " + zero + " b" + '\0';
    text += std::string("? an untracked") + '\0';

    const std::vector<Entry> expected = {
        {"a file", ChangeKind::Modified, true},
        {"a file", ChangeKind::Modified, false},
        {"an untracked", ChangeKind::Untracked, false},
        {"b", ChangeKind::Deleted, false},
        {"c", ChangeKind::Conflicted, false},
        {"new name", ChangeKind::Renamed, true},
    };
    QVERIFY(entries(parsePorcelainStatus(text)) == expected);
}

void StatusTest::everyKindOfChangeAtOnce() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path();
    QVERIFY(git(path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(path, {"config", "user.name", "Test"}));
    QVERIFY(git(path, {"config", "user.email", "test@localhost"}));
    for (const QString& name : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
                                QStringLiteral("d"), QStringLiteral("e"),
                                QStringLiteral("sp ace.txt"), QStringLiteral("юникод.txt"),
                                QStringLiteral("link")}) {
        QVERIFY(writeFile(path, name, "base\n"));
    }
    QVERIFY(writeFile(path, QStringLiteral(".gitignore"), "*.log\n"));
    QVERIFY(git(path, {"add", "-A"}));
    QVERIFY(git(path, {"commit", "-q", "-m", "base"}));

    QVERIFY(git(path, {"checkout", "-q", "-b", "other"}));
    QVERIFY(writeFile(path, QStringLiteral("c"), "other\n"));
    QVERIFY(git(path, {"commit", "-q", "-am", "other"}));
    QVERIFY(git(path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(path, QStringLiteral("c"), "main\n"));
    QVERIFY(git(path, {"commit", "-q", "-am", "main"}));
    QVERIFY(!git(path, {"merge", "-q", "other"}));

    QVERIFY(writeFile(path, QStringLiteral("a"), "staged\n"));
    QVERIFY(git(path, {"add", "a"}));
    QVERIFY(writeFile(path, QStringLiteral("a"), "staged\nunstaged\n"));
    QVERIFY(writeFile(path, QStringLiteral("b"), "changed\n"));
    QVERIFY(git(path, {"add", "b"}));
    QVERIFY(QFile::remove(QDir(path).filePath(QStringLiteral("d"))));
    QVERIFY(git(path, {"mv", "e", "e-renamed"}));
    QVERIFY(writeFile(path, QStringLiteral("new"), "new\n"));
    QVERIFY(git(path, {"add", "new"}));
    QVERIFY(writeFile(path, QStringLiteral("untracked/deep/u.txt"), "u\n"));
    QVERIFY(writeFile(path, QStringLiteral("ignored.log"), "log\n"));
    QVERIFY(writeFile(path, QStringLiteral("sp ace.txt"), "changed\n"));
    QVERIFY(writeFile(path, QStringLiteral("юникод.txt"), "changed\n"));
    QVERIFY(QFile::remove(QDir(path).filePath(QStringLiteral("link"))));
    QVERIFY(QFile::link(QStringLiteral("a"), QDir(path).filePath(QStringLiteral("link"))));
    QVERIFY(writeFile(path, QStringLiteral("intent.txt"), "intent\n"));
    QVERIFY(git(path, {"add", "-N", "intent.txt"}));

    RepoSession session;
    QVERIFY(session.open(path));
    const std::vector<Entry> expected = {
        {"a", ChangeKind::Modified, true},
        {"a", ChangeKind::Modified, false},
        {"b", ChangeKind::Modified, true},
        {"c", ChangeKind::Conflicted, false},
        {"d", ChangeKind::Deleted, false},
        {"e-renamed", ChangeKind::Renamed, true},
        // An intent to add is not staged content: git calls it an unstaged addition.
        {"intent.txt", ChangeKind::Added, false},
        {"link", ChangeKind::TypeChange, false},
        {"new", ChangeKind::Added, true},
        {"sp ace.txt", ChangeKind::Modified, false},
        {"untracked/deep/u.txt", ChangeKind::Untracked, false},
        {"юникод.txt", ChangeKind::Modified, false},
    };
    const std::vector<Entry> actual = entries(session.status());
    if (actual != expected) {
        for (const Entry& entry : actual) {
            qWarning() << std::get<0>(entry).c_str() << static_cast<int>(std::get<1>(entry))
                       << std::get<2>(entry);
        }
    }
    QVERIFY(actual == expected);
}

QTEST_GUILESS_MAIN(StatusTest)
#include "test_status.moc"
