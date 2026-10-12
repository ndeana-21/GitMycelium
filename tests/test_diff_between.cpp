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
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
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
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

/// The `added removed` pair `git diff --numstat` reports for one path.
QPair<int, int> numstat(const QString& dir, const QString& base, const QString& target,
                        const QString& path) {
    const QString line =
        gitOut(dir, {"diff", "--numstat", base, target, "--", path});
    if (line.isEmpty()) {
        return {0, 0};
    }
    const QStringList fields = line.split(QLatin1Char('\t'));
    return {fields.value(0).toInt(), fields.value(1).toInt()};
}

const QStringList kIdentity{"-c", "user.name=Test", "-c", "user.email=test@localhost"};

} // namespace

class DiffBetweenTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void changedFilesMatchTheGitBinary();
    void aFileDiffMatchesTheGitBinary();
    void theOrderOfThePairMatters();
    void anEmptyBaseReadsEveryPathAsAdded();
    void restoreFilePutsAnOldVersionBack();
    void restoreFileCanTargetTheIndex();

private:
    bool commit(const QString& message);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    QString m_c1;
    QString m_c3;
};

void DiffBetweenTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));

    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("one\ntwo\nthree\n")));
    QVERIFY(commit(QStringLiteral("c1")));
    m_c1 = gitOut(m_path, {"rev-parse", "HEAD"});

    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("one\nTWO\nthree\n")));
    QVERIFY(writeFile(m_path, QStringLiteral("b.txt"), QStringLiteral("brand new\n")));
    QVERIFY(commit(QStringLiteral("c2")));

    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("one\nTWO\nthree\nfour\n")));
    QVERIFY(commit(QStringLiteral("c3")));
    m_c3 = gitOut(m_path, {"rev-parse", "HEAD"});
}

void DiffBetweenTest::cleanup() {
    m_dir.reset();
}

bool DiffBetweenTest::commit(const QString& message) {
    if (!git(m_path, {"add", "-A"})) {
        return false;
    }
    QStringList args = kIdentity;
    args << "commit" << "-q" << "-m" << message;
    return git(m_path, args);
}

void DiffBetweenTest::changedFilesMatchTheGitBinary() {
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const std::vector<ChangedFile> ours = session.changedFilesBetween(m_c1, m_c3);
    const QStringList theirs = gitOut(m_path, {"diff", "--name-only", m_c1, m_c3})
                                   .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(ours.size(), static_cast<std::size_t>(theirs.size()));
    QStringList ourPaths;
    for (const ChangedFile& file : ours) {
        ourPaths << QString::fromStdString(file.path);
    }
    ourPaths.sort();
    QStringList sorted = theirs;
    sorted.sort();
    QCOMPARE(ourPaths, sorted);
}

void DiffBetweenTest::aFileDiffMatchesTheGitBinary() {
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const FileDiff ours = session.fileDiffBetween(m_c1, m_c3, QStringLiteral("a.txt"));
    const QPair<int, int> expected = numstat(m_path, m_c1, m_c3, QStringLiteral("a.txt"));
    QCOMPARE(ours.added, expected.first);
    QCOMPARE(ours.removed, expected.second);
    QVERIFY(!ours.hunks.empty());
}

void DiffBetweenTest::theOrderOfThePairMatters() {
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const FileDiff forward = session.fileDiffBetween(m_c1, m_c3, QStringLiteral("a.txt"));
    const FileDiff backward = session.fileDiffBetween(m_c3, m_c1, QStringLiteral("a.txt"));
    QCOMPARE(backward.added, forward.removed);
    QCOMPARE(backward.removed, forward.added);
}

void DiffBetweenTest::anEmptyBaseReadsEveryPathAsAdded() {
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const std::vector<ChangedFile> files = session.changedFilesBetween(QString(), m_c3);
    QVERIFY(files.size() >= 2);
    const FileDiff added = session.fileDiffBetween(QString(), m_c3, QStringLiteral("b.txt"));
    QCOMPARE(added.removed, 0);
    QVERIFY(added.added > 0);
}

void DiffBetweenTest::restoreFilePutsAnOldVersionBack() {
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("tampered\n")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.restoreFile(m_c1, QStringLiteral("a.txt"), /*staged=*/false, &error),
             qPrintable(error));

    QCOMPARE(readFile(m_path, QStringLiteral("a.txt")), QStringLiteral("one\ntwo\nthree\n"));
    // Working-tree restore does not stage a change: it matches HEAD's c3 blob? No —
    // c1's text differs from c3's, so it now shows as a working-tree modification.
    QVERIFY(gitOut(m_path, {"diff"}).contains(QStringLiteral("a.txt")));
}

void DiffBetweenTest::restoreFileCanTargetTheIndex() {
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("tampered\n")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.restoreFile(m_c1, QStringLiteral("a.txt"), /*staged=*/true, &error),
             qPrintable(error));

    // The index holds c1's version; the working tree still holds the tampered text.
    QCOMPARE(gitOut(m_path, {"show", ":a.txt"}), QStringLiteral("one\ntwo\nthree"));
    QCOMPARE(readFile(m_path, QStringLiteral("a.txt")), QStringLiteral("tampered\n"));
}

QTEST_MAIN(DiffBetweenTest)
#include "test_diff_between.moc"
