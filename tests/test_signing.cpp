/// Signing uses an SSH key generated for the test (`gpg.format=ssh`), so git produces
/// a real signature and `%G?` really answers; nothing is faked and nothing touches the
/// network. The test skips where `ssh-keygen` is not installed.

#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

ProcessResult gitRun(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString());
}

bool git(const QString& dir, const QStringList& args) {
    return gitRun(dir, args).exitCode == 0;
}

QString gitOut(const QString& dir, const QStringList& args) {
    return QString::fromStdString(gitRun(dir, args).out);
}

bool writeFile(const QString& dir, const QString& name, const QByteArray& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content) == content.size();
}

} // namespace

class SigningTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void signingConfigRoundTrips();
    void anUnsignedCommitHasNoGpgsig();
    void aSignedCommitCarriesGpgsig();
    void signingFromTheConfigSignsWithoutAnExplicitFlag();
    void aSignedAmendKeepsTheTree();
    void verifyReportsTheSignature();
    void theWindowReadsAndWritesSigning();

private:
    bool writeFileNamed(const QString& name, const QString& content);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void SigningTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));

    // A real signature, from a key generated for this test.
    const QString sshKeygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
    if (sshKeygen.isEmpty()) {
        QSKIP("ssh-keygen is not installed; the signing path cannot be exercised");
    }
    const QString keyPath = QDir(m_path).filePath(QStringLiteral("signing_key"));
    QCOMPARE(QProcess::execute(sshKeygen,
                               {QStringLiteral("-t"), QStringLiteral("ed25519"),
                                QStringLiteral("-N"), QString(), QStringLiteral("-f"), keyPath,
                                QStringLiteral("-q")}),
             0);

    QFile publicKey(keyPath + QStringLiteral(".pub"));
    QVERIFY(publicKey.open(QIODevice::ReadOnly));
    const QString pub = QString::fromUtf8(publicKey.readAll()).trimmed();
    const QString allowedPath = QDir(m_path).filePath(QStringLiteral("allowed_signers"));
    QVERIFY(writeFileNamed(QStringLiteral("allowed_signers"),
                           QStringLiteral("test@localhost %1\n").arg(pub)));

    QVERIFY(git(m_path, {"config", "gpg.format", "ssh"}));
    QVERIFY(git(m_path, {"config", "user.signingkey", keyPath + QStringLiteral(".pub")}));
    QVERIFY(git(m_path, {"config", "gpg.ssh.allowedSignersFile", allowedPath}));
}

void SigningTest::cleanup() {
    m_dir.reset();
}

bool SigningTest::writeFileNamed(const QString& name, const QString& content) {
    return writeFile(m_path, name, content.toUtf8());
}

void SigningTest::signingConfigRoundTrips() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    QVERIFY(!session.commitSigningEnabled());
    QVERIFY2(session.setConfigValue(QStringLiteral("commit.gpgsign"), QStringLiteral("true"),
                                    &error),
             qPrintable(error));
    QVERIFY(session.commitSigningEnabled());
    QCOMPARE(session.configValue(QStringLiteral("commit.gpgsign")), QStringLiteral("true"));

    QVERIFY2(session.setConfigValue(QStringLiteral("commit.gpgsign"), QStringLiteral("false"),
                                    &error),
             qPrintable(error));
    QVERIFY(!session.commitSigningEnabled());

    // An empty value removes the key rather than storing an empty string.
    QVERIFY2(session.setConfigValue(QStringLiteral("gpg.program"), QString(), &error),
             qPrintable(error));
    QVERIFY(!git(m_path, {"config", "--get", "gpg.program"}));
}

void SigningTest::anUnsignedCommitHasNoGpgsig() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));
    QVERIFY(writeFileNamed(QStringLiteral("a.txt"), QStringLiteral("a\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(session.createCommit(QStringLiteral("base"), nullptr, &error));
    QVERIFY(!gitOut(m_path, {"cat-file", "commit", "HEAD"}).contains(QStringLiteral("gpgsig")));
}

void SigningTest::aSignedCommitCarriesGpgsig() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    QVERIFY(writeFileNamed(QStringLiteral("a.txt"), QStringLiteral("a\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QString id;
    QVERIFY2(session.createCommit(QStringLiteral("signed"), true, &id, &error), qPrintable(error));
    QVERIFY(!id.isEmpty());
    QVERIFY2(gitOut(m_path, {"cat-file", "commit", id}).contains(QStringLiteral("gpgsig")),
             qPrintable(gitOut(m_path, {"cat-file", "commit", id})));
}

void SigningTest::signingFromTheConfigSignsWithoutAnExplicitFlag() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));
    QVERIFY2(session.setConfigValue(QStringLiteral("commit.gpgsign"), QStringLiteral("true"),
                                    &error),
             qPrintable(error));

    QVERIFY(writeFileNamed(QStringLiteral("a.txt"), QStringLiteral("a\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QString id;
    QVERIFY2(session.createCommit(QStringLiteral("config signed"), &id, &error), qPrintable(error));
    QVERIFY2(gitOut(m_path, {"cat-file", "commit", id}).contains(QStringLiteral("gpgsig")),
             qPrintable(gitOut(m_path, {"cat-file", "commit", id})));
}

void SigningTest::aSignedAmendKeepsTheTree() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    QVERIFY(writeFileNamed(QStringLiteral("a.txt"), QStringLiteral("a\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QString first;
    QVERIFY2(session.createCommit(QStringLiteral("first"), &first, &error), qPrintable(error));
    const QString tree = gitOut(m_path, {"rev-parse", "HEAD^{tree}"}).trimmed();

    // A staged change that a rewrite must not fold in: `--only` keeps the tree.
    QVERIFY(writeFileNamed(QStringLiteral("a.txt"), QStringLiteral("a\nb\n")));
    QVERIFY(git(m_path, {"add", "-A"}));

    QVERIFY2(session.amendHeadMessage(QStringLiteral("first reworded"), true, &error),
             qPrintable(error));
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD^{tree}"}).trimmed(), tree);
    QVERIFY2(gitOut(m_path, {"cat-file", "commit", "HEAD"}).contains(QStringLiteral("gpgsig")),
             qPrintable(gitOut(m_path, {"cat-file", "commit", "HEAD"})));
    QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%s"}).trimmed(),
             QStringLiteral("first reworded"));
}

void SigningTest::verifyReportsTheSignature() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    QVERIFY(writeFileNamed(QStringLiteral("a.txt"), QStringLiteral("a\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(session.createCommit(QStringLiteral("base"), nullptr, &error));
    const CommitSignature none = session.commitSignature(QStringLiteral("HEAD"));
    QCOMPARE(QString::fromStdString(none.status), QStringLiteral("N"));

    QVERIFY(writeFileNamed(QStringLiteral("b.txt"), QStringLiteral("b\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QString signedId;
    QVERIFY2(session.createCommit(QStringLiteral("signed"), true, &signedId, &error),
             qPrintable(error));
    const CommitSignature info = session.commitSignature(signedId);
    QCOMPARE(QString::fromStdString(info.status), QStringLiteral("G"));
    QVERIFY2(QString::fromStdString(info.signer).contains(QStringLiteral("test@localhost")),
             qPrintable(QString::fromStdString(info.signer)));
}

void SigningTest::theWindowReadsAndWritesSigning() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QString before =
        window.signingForDiagnostics(QStringLiteral("status"), {}).join(QLatin1Char('\n'));
    QVERIFY2(before.contains(QStringLiteral("sign: false")), qPrintable(before));

    const QString enabled =
        window.signingForDiagnostics(QStringLiteral("enable"), {}).join(QLatin1Char('\n'));
    QVERIFY2(enabled.contains(QStringLiteral("enable: ok")), qPrintable(enabled));

    const QString after =
        window.signingForDiagnostics(QStringLiteral("status"), {}).join(QLatin1Char('\n'));
    QVERIFY2(after.contains(QStringLiteral("sign: true")), qPrintable(after));

    const QString key =
        window.signingForDiagnostics(QStringLiteral("key"), {QStringLiteral("ABC123")})
            .join(QLatin1Char('\n'));
    QVERIFY2(key.contains(QStringLiteral("key: ok")), qPrintable(key));
    QCOMPARE(gitOut(m_path, {"config", "--get", "user.signingkey"}).trimmed(),
             QStringLiteral("ABC123"));
}

QTEST_MAIN(SigningTest)
#include "test_signing.moc"
