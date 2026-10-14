#include "git/cli.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

using namespace gk;

class CliTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void stdoutAndStderrAreSeparate();
    void bytesSurviveIncludingNul();
    void messagesAreEnglishWhateverTheLocale();
    void theEnvironmentReachesGitItself();
    void aWorkDirWithShellCharactersNeedsNoQuoting();
    void argumentsArriveExactlyAsGiven();
    void stdinIsNotTheTerminal();
    void gitHasNoControllingTerminal();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::string m_path;
};

void CliTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path().toStdString();
    QCOMPARE(runGit({"init", "-q", "-b", "main"}, m_path).exitCode, 0);
}

void CliTest::cleanup() {
    m_dir.reset();
}

void CliTest::stdoutAndStderrAreSeparate() {
    const ProcessResult missing = runGit({"rev-parse", "--verify", "no-such-ref"}, m_path);
    QVERIFY(missing.exitCode != 0);
    QVERIFY(missing.out.empty());
    QVERIFY(!missing.err.empty());
    QCOMPARE(QString::fromStdString(failureText(missing)),
             QString::fromStdString(missing.err).section(QLatin1Char('\n'), 0, 0));

    // A warning on stderr must not land in the data a parser reads.
    const ProcessResult hash =
        runGit({"-c", "alias.both=!echo data; echo noise >&2", "both"}, m_path);
    QCOMPARE(hash.exitCode, 0);
    QCOMPARE(QString::fromStdString(hash.out), QStringLiteral("data\n"));
    QCOMPARE(QString::fromStdString(hash.err), QStringLiteral("noise\n"));
}

void CliTest::bytesSurviveIncludingNul() {
    QByteArray bytes("before");
    bytes.append('\0');
    bytes.append("after\n");
    QFile file(QDir(m_dir->path()).filePath(QStringLiteral("bin.dat")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    file.close();

    const ProcessResult hashed = runGit({"hash-object", "-w", "bin.dat"}, m_path);
    QCOMPARE(hashed.exitCode, 0);
    const std::string oid = hashed.out.substr(0, hashed.out.find('\n'));
    const ProcessResult blob = runGit({"cat-file", "blob", oid}, m_path);
    QCOMPARE(blob.exitCode, 0);
    QCOMPARE(QByteArray::fromStdString(blob.out), bytes);
}

void CliTest::messagesAreEnglishWhateverTheLocale() {
    const QByteArray saved = qgetenv("LC_ALL");
    qputenv("LC_ALL", "ru_RU.UTF-8");
    const ProcessResult run = runGit({"rev-parse", "--verify", "no-such-ref"}, m_path);
    if (saved.isNull()) {
        qunsetenv("LC_ALL");
    } else {
        qputenv("LC_ALL", saved);
    }
    QVERIFY2(QString::fromStdString(run.err).startsWith(QStringLiteral("fatal:")),
             run.err.c_str());
}

void CliTest::theEnvironmentReachesGitItself() {
    const ProcessResult run = runGit(
        {"-c", "alias.env=!echo $GIT_OPTIONAL_LOCKS $GIT_TERMINAL_PROMPT $LANGUAGE", "env"},
        m_path);
    QCOMPARE(run.exitCode, 0);
    QCOMPARE(QString::fromStdString(run.out), QStringLiteral("0 0 en\n"));
}

void CliTest::aWorkDirWithShellCharactersNeedsNoQuoting() {
#ifdef Q_OS_WIN
    // A quote cannot be part of a Windows file name.
    const QString odd = m_dir->path() + QStringLiteral("/a dir $HOME 'q %PATH% `x` & ^");
#else
    const QString odd = m_dir->path() + QStringLiteral("/a dir $HOME 'q\" `x`");
#endif
    QVERIFY(QDir().mkpath(odd));
    const std::string path = odd.toStdString();
    QCOMPARE(runGit({"init", "-q"}, path).exitCode, 0);
    const ProcessResult top = runGit({"rev-parse", "--show-toplevel"}, path);
    QCOMPARE(top.exitCode, 0);
    QCOMPARE(QString::fromStdString(top.out).trimmed(), QDir(odd).canonicalPath());
}

void CliTest::argumentsArriveExactlyAsGiven() {
    // Windows hands a child one command line, which the child splits again: spaces,
    // quotes and backslashes before a quote must come back as they were sent.
    const std::vector<std::string> sent = {"plain",   "two words", "", "quote\"inside",
                                           "trail\\", "slash\\\"quote", "tab\there",
                                           "\u0434\u0438\u0440"};
    std::vector<std::string> args = {"-c", "alias.args=!printf '[%s]\\n'", "args"};
    args.insert(args.end(), sent.begin(), sent.end());
    const ProcessResult run = runGit(args, m_path);
    QCOMPARE(run.exitCode, 0);
    std::string expected;
    for (const std::string& arg : sent) {
        expected += "[" + arg + "]\n";
    }
    QCOMPARE(QString::fromStdString(run.out), QString::fromStdString(expected));
}

void CliTest::stdinIsNotTheTerminal() {
    const ProcessResult run = runGit({"-c", "alias.in=!cat", "in"}, m_path);
    QCOMPARE(run.exitCode, 0);
    QVERIFY(run.out.empty());
}

void CliTest::gitHasNoControllingTerminal() {
    // ssh asks about an unknown host key on /dev/tty, not on stdin; with the
    // terminal the GUI was started from, that question waited forever. Opening the
    // controlling terminal must fail inside anything git starts.
    const ProcessResult tty =
        runGit({"-c", "alias.tty=!sh -c 'exec 3</dev/tty' 2>/dev/null", "tty"}, m_path);
    QVERIFY(tty.exitCode != 0);

#ifdef Q_OS_LINUX
    // The same, by the session: git leads a new one, so it is not ours. (Field 6 of
    // /proc/<pid>/stat, after the command name in parentheses.)
    const ProcessResult stat =
        runGit({"-c", "alias.stat=!cat /proc/$PPID/stat", "stat"}, m_path);
    QCOMPARE(stat.exitCode, 0);
    const QString text = QString::fromStdString(stat.out);
    const QStringList fields =
        text.mid(text.lastIndexOf(QLatin1Char(')')) + 2).split(QLatin1Char(' '));
    QVERIFY(fields.size() > 3);
    QVERIFY(fields.at(3).toLongLong() != static_cast<long long>(getsid(0)));
#endif
}

QTEST_GUILESS_MAIN(CliTest)
#include "test_cli.moc"
