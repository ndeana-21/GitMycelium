#include "git/cli.h"
#include "git/sparse.h"
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

bool writeFile(const QString& path, const QByteArray& content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return f.write(content) == content.size();
}

} // namespace

class SparseTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void parseDropsTheRootAndTrimsConeSlashes();
    void enablingKeepsTheChosenDirectoryAndRemovesTheRest();
    void disablingRestoresTheWholeTree();
    void topLevelDirectoriesComeFromHeadEvenWhenExcluded();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void SparseTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(QDir(m_path).mkpath(QStringLiteral("lib")));
    QVERIFY(QDir(m_path).mkpath(QStringLiteral("app")));
    QVERIFY(writeFile(m_path + "/lib/a.txt", "lib\n"));
    QVERIFY(writeFile(m_path + "/app/b.txt", "app\n"));
    QVERIFY(writeFile(m_path + "/top.txt", "top\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
}

void SparseTest::cleanup() {
    m_dir.reset();
}

void SparseTest::parseDropsTheRootAndTrimsConeSlashes() {
    const std::vector<std::string> parsed =
        parseSparseList("/\n/lib/\n/lib/sub/\n");
    QCOMPARE(parsed.size(), static_cast<std::size_t>(2));
    QCOMPARE(QString::fromStdString(parsed.at(0)), QStringLiteral("lib"));
    QCOMPARE(QString::fromStdString(parsed.at(1)), QStringLiteral("lib/sub"));
}

void SparseTest::enablingKeepsTheChosenDirectoryAndRemovesTheRest() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QVERIFY(!session.sparseCheckoutEnabled());

    QString error;
    QVERIFY2(session.setSparseCheckout({QStringLiteral("lib")}, &error), qPrintable(error));

    QVERIFY(session.sparseCheckoutEnabled());
    QCOMPARE(session.sparseCheckoutDirectories(), QStringList{QStringLiteral("lib")});
    QVERIFY(QFile::exists(m_path + "/lib/a.txt"));
    QVERIFY(!QFile::exists(m_path + "/app/b.txt"));
    QVERIFY(git(m_path, {"cat-file", "-e", "HEAD:app/b.txt"}));
}

void SparseTest::disablingRestoresTheWholeTree() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.setSparseCheckout({QStringLiteral("lib")}, &error), qPrintable(error));
    QVERIFY(!QFile::exists(m_path + "/app/b.txt"));

    QVERIFY2(session.setSparseCheckout({}, &error), qPrintable(error));
    QVERIFY(!session.sparseCheckoutEnabled());
    QVERIFY(QFile::exists(m_path + "/app/b.txt"));
}

void SparseTest::topLevelDirectoriesComeFromHeadEvenWhenExcluded() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.setSparseCheckout({QStringLiteral("lib")}, &error), qPrintable(error));

    QVERIFY(!QFile::exists(m_path + "/app/b.txt"));
    QCOMPARE(session.sparseCheckoutTopLevelDirectories(),
             (QStringList{QStringLiteral("app"), QStringLiteral("lib")}));
}

QTEST_MAIN(SparseTest)
#include "test_sparse.moc"
