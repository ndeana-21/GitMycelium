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

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

const QStringList kIdentity{"-c", "user.name=Test", "-c", "user.email=test@localhost"};

} // namespace

class ReflogTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theNewestEntryIsHeadAndNamesItsCommit();
    void aCheckoutIsRecordedWithItsAction();
    void anEmptyReflogIsNotAnError();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void ReflogTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
}

void ReflogTest::cleanup() {
    m_dir.reset();
}

void ReflogTest::theNewestEntryIsHeadAndNamesItsCommit() {
    QVERIFY(writeFile(m_path, "a.txt", "one\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, kIdentity + QStringList{"commit", "-q", "-m", "first commit"}));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const std::vector<ReflogEntry> entries = session.reflog(10);
    QVERIFY(!entries.empty());
    QCOMPARE(QString::fromStdString(entries.front().selector), QStringLiteral("HEAD@{0}"));
    QCOMPARE(QString::fromStdString(entries.front().id),
             QString::fromStdString(session.info().headId));
    QCOMPARE(QString::fromStdString(entries.front().action), QStringLiteral("commit (initial)"));
    QVERIFY(QString::fromStdString(entries.front().message)
                .contains(QStringLiteral("first commit")));
}

void ReflogTest::aCheckoutIsRecordedWithItsAction() {
    QVERIFY(writeFile(m_path, "a.txt", "one\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, kIdentity + QStringList{"commit", "-q", "-m", "base"}));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));

    RepoSession session;
    QVERIFY(session.open(m_path));
    const std::vector<ReflogEntry> entries = session.reflog(10);

    bool sawCheckout = false;
    for (const ReflogEntry& entry : entries) {
        if (entry.action == "checkout") {
            sawCheckout = true;
            QVERIFY(entry.message.find("feature") != std::string::npos);
        }
    }
    QVERIFY2(sawCheckout, "moving to a new branch must be in the reflog as a checkout");
}

void ReflogTest::anEmptyReflogIsNotAnError() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QVERIFY(session.reflog(10).empty());
}

QTEST_MAIN(ReflogTest)
#include "test_reflog.moc"
