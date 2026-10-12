/// Binary conflicts, against real repositories. A binary file has no text to
/// merge, so the only honest resolution is to keep one whole side — and the
/// failure this pins is the old one: "Take ours" fed an empty string to the text
/// path and staged an empty file over the binary.

#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"

#include <QByteArray>
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
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

bool writeBytes(const QString& dir, const QString& name, const QByteArray& bytes) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& dir, const QString& name) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

/// Bytes that make git call the file binary: a NUL in the first few bytes.
QByteArray bin(const char* text) {
    QByteArray bytes;
    bytes.append(char(0x00));
    bytes.append(char(0x01));
    bytes.append(char(0x02));
    bytes.append(text);
    bytes.append(char(0xff));
    return bytes;
}

const QByteArray kBase = bin("base");
const QByteArray kMain = bin("MAIN");
const QByteArray kFeature = bin("FEATURE");

} // namespace

class ConflictBinaryTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aBinaryConflictHasNoText();
    void keepingOursWritesAndStagesThoseBytes();
    void keepingTheirsWritesAndStagesThoseBytes();
    void keepingBaseWritesAndStagesThoseBytes();
    void aConflictWithOneBinarySideHasNoText();
    void aHeadingUnderlineGetsMarkersLongerThanItself();

private:
    bool commit(const QString& message);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void ConflictBinaryTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));

    QVERIFY(writeBytes(m_path, QStringLiteral("bin.dat"), kBase));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(commit(QStringLiteral("base")));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeBytes(m_path, QStringLiteral("bin.dat"), kFeature));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(commit(QStringLiteral("feature")));

    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeBytes(m_path, QStringLiteral("bin.dat"), kMain));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(commit(QStringLiteral("main")));

    // The merge is expected to conflict; the exit code is the point.
    git(m_path, {"merge", "feature"});
    QCOMPARE(gitOut(m_path, {"ls-files", "-u"}).split(QLatin1Char('\n')).size(), 3);
}

void ConflictBinaryTest::cleanup() {
    m_dir.reset();
}

bool ConflictBinaryTest::commit(const QString& message) {
    return git(m_path, {"-c", "user.name=Test", "-c", "user.email=test@localhost", "commit", "-q",
                        "-m", message});
}

void ConflictBinaryTest::aBinaryConflictHasNoText() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    const std::vector<ConflictEntry> conflicts = session.conflicts();
    QCOMPARE(conflicts.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(conflicts[0].path), QStringLiteral("bin.dat"));
    QVERIFY(conflicts[0].kind == ConflictKind::BothModified);

    bool textAvailable = true;
    session.conflictMarkedContent(QStringLiteral("bin.dat"), &textAvailable);
    QVERIFY2(!textAvailable, "a binary conflict has no text to compare");

    QCOMPARE(session.blobSize(QString::fromStdString(conflicts[0].ours)),
             static_cast<long long>(kMain.size()));
    QCOMPARE(session.blobSize(QString::fromStdString(conflicts[0].theirs)),
             static_cast<long long>(kFeature.size()));
    QCOMPARE(session.blobSize(QString::fromStdString(conflicts[0].ancestor)),
             static_cast<long long>(kBase.size()));
}

void ConflictBinaryTest::keepingOursWritesAndStagesThoseBytes() {
    const QString stageOid = gitOut(m_path, {"rev-parse", ":2:bin.dat"});
    QVERIFY(!stageOid.isEmpty());

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));
    QVERIFY2(session.keepConflictSide(QStringLiteral("bin.dat"), ConflictSide::Ours, &error),
             qPrintable(error));

    QCOMPARE(readBytes(m_path, QStringLiteral("bin.dat")), kMain);
    QCOMPARE(gitOut(m_path, {"rev-parse", ":bin.dat"}), stageOid);
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).isEmpty());
}

void ConflictBinaryTest::keepingTheirsWritesAndStagesThoseBytes() {
    const QString stageOid = gitOut(m_path, {"rev-parse", ":3:bin.dat"});
    QVERIFY(!stageOid.isEmpty());

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));
    QVERIFY2(session.keepConflictSide(QStringLiteral("bin.dat"), ConflictSide::Theirs, &error),
             qPrintable(error));

    QCOMPARE(readBytes(m_path, QStringLiteral("bin.dat")), kFeature);
    QCOMPARE(gitOut(m_path, {"rev-parse", ":bin.dat"}), stageOid);
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).isEmpty());
}

void ConflictBinaryTest::keepingBaseWritesAndStagesThoseBytes() {
    const QString stageOid = gitOut(m_path, {"rev-parse", ":1:bin.dat"});
    QVERIFY(!stageOid.isEmpty());

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));
    QVERIFY2(session.keepConflictSide(QStringLiteral("bin.dat"), ConflictSide::Base, &error),
             qPrintable(error));

    QCOMPARE(readBytes(m_path, QStringLiteral("bin.dat")), kBase);
    QCOMPARE(gitOut(m_path, {"rev-parse", ":bin.dat"}), stageOid);
    QVERIFY(gitOut(m_path, {"ls-files", "-u"}).isEmpty());
}

namespace {

bool conflictedRepository(const QString& path, const QString& name, const QByteArray& base,
                          const QByteArray& ours, const QByteArray& theirs) {
    const auto commitAll = [&](const char* message) {
        return git(path, {"add", "-A"}) &&
               git(path, {"-c", "user.name=Test", "-c", "user.email=test@localhost", "commit",
                          "-q", "-m", QString::fromLatin1(message)});
    };
    if (!git(path, {"init", "-q", "-b", "main"}) || !writeBytes(path, name, base) ||
        !commitAll("base") || !git(path, {"checkout", "-q", "-b", "feature"}) ||
        !writeBytes(path, name, theirs) || !commitAll("feature") ||
        !git(path, {"checkout", "-q", "main"}) || !writeBytes(path, name, ours) ||
        !commitAll("main")) {
        return false;
    }
    git(path, {"merge", "feature"});
    return !gitOut(path, {"ls-files", "-u"}).isEmpty();
}

} // namespace

void ConflictBinaryTest::aConflictWithOneBinarySideHasNoText() {
    QTemporaryDir dir;
    QVERIFY(conflictedRepository(dir.path(), QStringLiteral("mixed.dat"), "base text\n",
                                 "our text\n", bin("FEATURE")));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(dir.path(), &error), qPrintable(error));
    bool textAvailable = true;
    session.conflictMarkedContent(QStringLiteral("mixed.dat"), &textAvailable);
    QVERIFY2(!textAvailable, "text over a binary side would replace the binary when saved");
}

void ConflictBinaryTest::aHeadingUnderlineGetsMarkersLongerThanItself() {
    QTemporaryDir dir;
    QVERIFY(conflictedRepository(dir.path(), QStringLiteral("doc.rst"),
                                 "Intro\n\nTitle\n=======\n\nbody\n",
                                 "Intro\n\nOurs\n=======\n\nbody\n",
                                 "Intro\n\nTheirs\n=======\n\nbody\n"));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(dir.path(), &error), qPrintable(error));
    bool textAvailable = false;
    const QString marked = session.conflictMarkedContent(QStringLiteral("doc.rst"), &textAvailable);
    QVERIFY(textAvailable);
    QVERIFY2(marked.contains(QStringLiteral("<<<<<<<< ")), qPrintable(marked));
    QVERIFY2(marked.contains(QStringLiteral("\n========\n")), qPrintable(marked));
    QVERIFY2(marked.contains(QStringLiteral("\n=======\n\nbody")), qPrintable(marked));
}

QTEST_MAIN(ConflictBinaryTest)
#include "test_conflict_binary.moc"
