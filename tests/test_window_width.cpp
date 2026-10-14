/// The window's width belongs to the reader. Opening a file with a long path, or a page
/// that is not on screen, must not push its minimum wider.

#include "git/cli.h"
#include "git/repository.h"
#include "ui/commit_details_panel.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
#include <QSplitter>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).exitCode == 0;
}

bool writeFile(const QString& path, const QString& content) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content.toUtf8()) == content.toUtf8().size();
}

QSplitter* columns(const QWidget& window) {
    for (QSplitter* splitter : window.findChildren<QSplitter*>()) {
        if (splitter->orientation() == Qt::Horizontal && splitter->count() == 3) {
            return splitter;
        }
    }
    return nullptr;
}

} // namespace

class WindowWidthTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_config;
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_repo;
    QString m_longPath;

private slots:
    void initTestCase();
    void init();

    void aHiddenPageDoesNotWidenTheWindow();
    void openingALongPathKeepsTheWidth();
};

void WindowWidthTest::initTestCase() {
    m_config = std::make_unique<QTemporaryDir>();
    qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
    Repository::initLibrary();
}

void WindowWidthTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    m_repo = QDir(m_dir->path()).filePath(QStringLiteral("repo"));
    QVERIFY(QDir().mkpath(m_repo));
    QVERIFY(git(m_repo, {"init", "-q", "-b", "main"}));
    m_longPath = QStringLiteral("a/very/deep/directory/structure/that/goes/on/and/on/for/a/"
                                "while/before/it/finally/reaches/the/file/it/names/"
                                "with_a_long_descriptive_name_too.txt");
    QVERIFY(writeFile(QDir(m_repo).filePath(QStringLiteral("short.txt")), QStringLiteral("x\n")));
    QVERIFY(writeFile(QDir(m_repo).filePath(m_longPath), QStringLiteral("x\n")));
    QVERIFY(git(m_repo, {"add", "-A"}));
    QVERIFY(git(m_repo, {"commit", "-qm", "first"}));
    QVERIFY(writeFile(QDir(m_repo).filePath(QStringLiteral("short.txt")), QStringLiteral("y\n")));
    QVERIFY(writeFile(QDir(m_repo).filePath(m_longPath), QStringLiteral("y\n")));
}

void WindowWidthTest::aHiddenPageDoesNotWidenTheWindow() {
    MainWindow window;
    window.setHeadless(true);
    QVERIFY(window.openRepository(m_repo));
    // The merge editor and the diff are behind the graph; a laptop screen must fit.
    QVERIFY2(window.minimumSizeHint().width() < 1000,
             qPrintable(QString::number(window.minimumSizeHint().width())));
}

void WindowWidthTest::openingALongPathKeepsTheWidth() {
    MainWindow window;
    window.setHeadless(true);
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openRepository(m_repo));
    window.selectCommitRow(0);
    auto* details = window.findChild<CommitDetailsPanel*>();
    QVERIFY(details);

    emit details->fileSelected(QStringLiteral("short.txt"), false);
    window.showDiffPage();
    QTest::qWait(50);
    const int before = window.minimumSizeHint().width();
    QSplitter* history = columns(window);
    QVERIFY(history);
    const QList<int> sizesBefore = history->sizes();

    emit details->fileSelected(m_longPath, false);
    QTest::qWait(50);
    QCOMPARE(window.minimumSizeHint().width(), before);
    QCOMPARE(window.width(), 1000);
    QCOMPARE(history->sizes(), sizesBefore);
}

QTEST_MAIN(WindowWidthTest)
#include "test_window_width.moc"
