#include "git/cli.h"
#include "ui/graph_columns.h"
#include "ui/main_window.h"

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
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    return f.write(content) == content.size();
}

QString field(const QStringList& report, const QString& key) {
    for (const QString& line : report) {
        if (line.startsWith(key + QStringLiteral(": "))) {
            return line.section(QStringLiteral(": "), 1);
        }
    }
    return QString();
}

QString fieldOf(const QStringList& report, const QString& column, const QString& part) {
    for (const QString& line : report) {
        if (!line.startsWith(column + QStringLiteral(": "))) {
            continue;
        }
        for (const QString& token : line.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (token.startsWith(part + QLatin1Char('='))) {
                return token.section(QLatin1Char('='), 1);
            }
        }
    }
    return QString();
}

} // namespace

class GraphColumnsTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void richIsTheDefault();
    void textDropsTheGlyphsAndKeepsTheWidth();
    void compactNarrowsTheLabelColumn();
    void theModeIsRememberedPerRepository();

private:
    void buildRepo();
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void GraphColumnsTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    buildRepo();
}

void GraphColumnsTest::cleanup() {
    m_dir.reset();
}

void GraphColumnsTest::buildRepo() {
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(m_path + "/a.txt", "one\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, {"commit", "-qm", "first"}));
    QVERIFY(git(m_path, {"tag", "v1.0"}));
    QVERIFY(git(m_path, {"branch", "feature/x"}));
}

void GraphColumnsTest::richIsTheDefault() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    const QStringList report = window.columnsForDiagnostics(QStringLiteral("show"));
    QCOMPARE(field(report, QStringLiteral("mode")), QStringLiteral("rich"));
    QCOMPARE(field(report, QStringLiteral("icons")), QStringLiteral("yes"));
    QCOMPARE(fieldOf(report, QStringLiteral("refs"), QStringLiteral("w")), QStringLiteral("260"));
}

void GraphColumnsTest::textDropsTheGlyphsAndKeepsTheWidth() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    const QStringList report = window.columnsForDiagnostics(QStringLiteral("text"));
    QCOMPARE(field(report, QStringLiteral("mode")), QStringLiteral("text"));
    QCOMPARE(field(report, QStringLiteral("icons")), QStringLiteral("no"));
    QCOMPARE(fieldOf(report, QStringLiteral("refs"), QStringLiteral("w")), QStringLiteral("260"));
}

void GraphColumnsTest::compactNarrowsTheLabelColumn() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    const QStringList report = window.columnsForDiagnostics(QStringLiteral("compact"));
    QCOMPARE(field(report, QStringLiteral("mode")), QStringLiteral("compact"));
    QCOMPARE(field(report, QStringLiteral("icons")), QStringLiteral("no"));
    QCOMPARE(fieldOf(report, QStringLiteral("refs"), QStringLiteral("w")), QStringLiteral("170"));
}

void GraphColumnsTest::theModeIsRememberedPerRepository() {
    {
        MainWindow window;
        QVERIFY(window.openRepository(m_path));
        window.columnsForDiagnostics(QStringLiteral("compact"));
    }
    // A second window on the same checkout sees the choice: it is stored under the
    // repository's key, not left in the window that made it.
    MainWindow reopened;
    QVERIFY(reopened.openRepository(m_path));
    const QStringList report = reopened.columnsForDiagnostics(QStringLiteral("show"));
    QCOMPARE(field(report, QStringLiteral("mode")), QStringLiteral("compact"));
    QCOMPARE(field(report, QStringLiteral("icons")), QStringLiteral("no"));
}

QTEST_MAIN(GraphColumnsTest)
#include "test_graph_columns.moc"
