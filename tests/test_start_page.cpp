/// The start page reports the request and the window acts on it, so a widget test can
/// drive it without a repository at all.

#include "ui/start_page.h"

#include <QApplication>
#include <QFont>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QtTest>

using namespace gk;

namespace {

QVector<RecentRepository> twoEntries() {
    RecentRepository one;
    one.path = QStringLiteral("/tmp/one");
    one.name = QStringLiteral("one");
    one.lastUsed = QDateTime(QDate(2024, 5, 1), QTime(9, 30));
    RecentRepository two;
    two.path = QStringLiteral("/tmp/two");
    two.name = QStringLiteral("two");
    two.lastUsed = QDateTime(QDate(2024, 6, 2), QTime(18, 5));
    return {one, two};
}

} // namespace

class StartPageTest : public QObject {
    Q_OBJECT

private slots:
    void listsTheRecentRepositories();
    void anEmptyListShowsTheEmptyLine();
    void theButtonsEmitTheirSignals();
    void activatingARowReportsItsPath();
    void theTitleIsBiggerThanTheBaseFont();
    void theTitleNamesTheApplication();
};

void StartPageTest::listsTheRecentRepositories() {
    StartPage page;
    page.setRecentRepositories(twoEntries());
    auto* list = page.findChild<QTreeWidget*>(QStringLiteral("recentList"));
    QVERIFY(list);
    QCOMPARE(list->topLevelItemCount(), 2);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("one"));
    QCOMPARE(list->topLevelItem(0)->text(1), QStringLiteral("/tmp/one"));
    QVERIFY(!list->topLevelItem(0)->text(2).isEmpty());
    QVERIFY(!page.findChild<QLabel*>(QStringLiteral("startPageRecentEmpty"))->isVisible());
}

void StartPageTest::anEmptyListShowsTheEmptyLine() {
    StartPage page;
    page.setRecentRepositories({});
    auto* list = page.findChild<QTreeWidget*>(QStringLiteral("recentList"));
    QCOMPARE(list->topLevelItemCount(), 0);
    QVERIFY(list->isHidden());
    auto* empty = page.findChild<QLabel*>(QStringLiteral("startPageRecentEmpty"));
    QVERIFY(empty);
    QVERIFY(!empty->isHidden());
}

void StartPageTest::theButtonsEmitTheirSignals() {
    StartPage page;
    QSignalSpy openSpy(&page, &StartPage::openRequested);
    QSignalSpy cloneSpy(&page, &StartPage::cloneRequested);
    QSignalSpy createSpy(&page, &StartPage::createRequested);

    page.findChild<QPushButton*>(QStringLiteral("startPageOpen"))->click();
    page.findChild<QPushButton*>(QStringLiteral("startPageClone"))->click();
    page.findChild<QPushButton*>(QStringLiteral("startPageCreate"))->click();

    QCOMPARE(openSpy.count(), 1);
    QCOMPARE(cloneSpy.count(), 1);
    QCOMPARE(createSpy.count(), 1);
}

void StartPageTest::activatingARowReportsItsPath() {
    StartPage page;
    page.setRecentRepositories(twoEntries());
    auto* list = page.findChild<QTreeWidget*>(QStringLiteral("recentList"));
    QTreeWidgetItem* item = list->topLevelItem(0);

    QSignalSpy activated(&page, &StartPage::recentActivated);
    // Emitting the view's own signal is the deterministic way to test what the page
    // wired to it: a synthetic double-click depends on hit-testing and geometry.
    QVERIFY(QMetaObject::invokeMethod(list, "itemDoubleClicked",
                                      Q_ARG(QTreeWidgetItem*, item), Q_ARG(int, 0)));
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.first().at(0).toString(), QStringLiteral("/tmp/one"));
}

void StartPageTest::theTitleIsBiggerThanTheBaseFont() {
    // The application font is set in pixels (`theme::applyApplicationTheme`), so
    // `pointSizeF()` is -1 and a title that doubles it asks Qt for -2: a warning, and
    // a title left the same size as everything else. Pin that the title comes out
    // bigger whichever unit the application font uses.
    struct FontGuard {
        QFont saved;
        FontGuard() : saved(QApplication::font()) {}
        ~FontGuard() { QApplication::setFont(saved); }
    } guard;

    QFont pixelFont = QApplication::font();
    pixelFont.setPixelSize(13);
    QApplication::setFont(pixelFont);

    StartPage page;
    auto* title = page.findChild<QLabel*>(QStringLiteral("startPageTitle"));
    QVERIFY(title);
    QVERIFY(title->font().bold());
    QVERIFY(title->font().pixelSize() > 0);
    QVERIFY2(title->font().pixelSize() > 13,
             qPrintable(QString::number(title->font().pixelSize())));
}

void StartPageTest::theTitleNamesTheApplication() {
    // The product name is "GitMycelium" wherever it is read; the binary and the
    // settings identity stay lowercase.
    StartPage page;
    auto* title = page.findChild<QLabel*>(QStringLiteral("startPageTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("GitMycelium"));
}

QTEST_MAIN(StartPageTest)
#include "test_start_page.moc"
