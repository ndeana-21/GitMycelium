#include "ui/badge_flight.h"
#include "ui/graph_operation_panel.h"

#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

using namespace gk;

class BadgeFlightTest : public QObject {
    Q_OBJECT

private slots:
    void aFreshBadgeIsHome();
    void theBadgeArrivesAtTheAnchor();
    void aLandingBringsItHome();
    void landingTwiceIsHarmless();
    void landingBeforeAnyLiftIsHarmless();
    void landingMidFlightReturnsFromWhereItIs();
    void landingImmediatelyPutsItHome();

    void theBadgeParksBesideTheOtherBadgeNotOverIt();
    void theListOpensToTheRightOfTheBadge();
    void theListIsPressedBackInsideItsRoom();

    void thePanelReportsOneSignalPerPresentation();
    void theListAsksForTheOperationTheReaderClicked();
    void theInteractiveEntryIsLiveAndReportsItself();
};

void BadgeFlightTest::aFreshBadgeIsHome() {
    BadgeFlight flight;
    QVERIFY(flight.grounded());
    QVERIFY(!flight.lifted());
    QVERIFY(!flight.flying());
}

void BadgeFlightTest::theBadgeArrivesAtTheAnchor() {
    BadgeFlight flight;
    flight.lift(QStringLiteral("feature"), QPoint(10, 10), QPoint(200, 60));
    QVERIFY(flight.lifted());
    QVERIFY(flight.flying());

    flight.advance(BadgeFlight::kLiftMs);
    QCOMPARE(flight.position(), QPoint(200, 60));
    QVERIFY(flight.lifted());
    QVERIFY(!flight.flying());
}

void BadgeFlightTest::aLandingBringsItHome() {
    BadgeFlight flight;
    flight.lift(QStringLiteral("feature"), QPoint(10, 10), QPoint(200, 60));
    flight.advance(BadgeFlight::kLiftMs);
    flight.land();
    QVERIFY(flight.flying());

    flight.advance(BadgeFlight::kLandMs);
    QVERIFY(flight.grounded());
    QCOMPARE(flight.position(), QPoint(10, 10));
}

void BadgeFlightTest::landingTwiceIsHarmless() {
    BadgeFlight flight;
    flight.lift(QStringLiteral("feature"), QPoint(10, 10), QPoint(200, 60));
    flight.advance(BadgeFlight::kLiftMs);
    flight.land();
    flight.land();
    flight.advance(BadgeFlight::kLandMs);
    QVERIFY(flight.grounded());
    QCOMPARE(flight.position(), QPoint(10, 10));
}

void BadgeFlightTest::landingBeforeAnyLiftIsHarmless() {
    BadgeFlight flight;
    flight.land();
    QVERIFY(flight.grounded());
    flight.advance(50);
    QVERIFY(flight.grounded());
}

void BadgeFlightTest::landingMidFlightReturnsFromWhereItIs() {
    BadgeFlight flight;
    flight.lift(QStringLiteral("feature"), QPoint(0, 0), QPoint(100, 0));
    flight.advance(BadgeFlight::kLiftMs / 2);
    const QPoint midway = flight.position();
    QVERIFY(midway.x() > 0 && midway.x() < 100);

    flight.land();
    // No jump: the return starts at the point it had reached, not back at the anchor.
    QCOMPARE(flight.position(), midway);
    flight.advance(BadgeFlight::kLandMs);
    QVERIFY(flight.grounded());
    QCOMPARE(flight.position(), QPoint(0, 0));
}

void BadgeFlightTest::landingImmediatelyPutsItHome() {
    BadgeFlight flight;
    flight.lift(QStringLiteral("feature"), QPoint(5, 5), QPoint(300, 300));
    flight.advance(BadgeFlight::kLiftMs / 3);
    flight.landImmediately();
    QVERIFY(flight.grounded());
    QCOMPARE(flight.position(), QPoint(5, 5));
}

void BadgeFlightTest::theBadgeParksBesideTheOtherBadgeNotOverIt() {
    // The rule: by the badge, not by the text. A centre placed at the pill's right
    // edge would put half the lifted badge on top of the one it was dropped on.
    const QRect target(100, 40, 120, 20);
    const int width = 90;
    const QPoint centre = parkedBadgeCentre(target, width);

    const QRect parked(centre.x() - width / 2, centre.y() - 10, width, 20);
    QVERIFY2(parked.left() >= target.right(), "the badge overlaps the one it was dropped on");
    QCOMPARE(parked.left(), target.right() + kBadgeGap);
    QCOMPARE(centre.y(), target.center().y());
}

void BadgeFlightTest::theListOpensToTheRightOfTheBadge() {
    const QPoint centre(200, 50);
    const int width = 90;
    const QPoint at = listTopLeft(centre, width);
    QVERIFY2(at.x() >= centre.x() + width / 2, "the list overlaps the badge it belongs to");
}

void BadgeFlightTest::theListIsPressedBackInsideItsRoom() {
    GraphOperationPanel panel;
    const QRect room(0, 0, 800, 600);
    panel.present(QStringLiteral("feature/conflict-a"), QStringLiteral("main"), QPoint(780, 560),
                  room);
    QVERIFY(panel.x() + panel.width() <= room.right() + 1);
    QVERIFY(panel.y() + panel.height() <= room.bottom() + 1);
    QVERIFY(panel.x() >= room.left());
    QVERIFY(panel.y() >= room.top());
}

void BadgeFlightTest::thePanelReportsOneSignalPerPresentation() {
    GraphOperationPanel panel;
    QSignalSpy chosen(&panel, &GraphOperationPanel::chosen);
    QSignalSpy dismissed(&panel, &GraphOperationPanel::dismissed);

    panel.present(QStringLiteral("feature"), QStringLiteral("main"), QPoint(0, 0));
    panel.dismiss();
    panel.dismiss();
    QCOMPARE(dismissed.count(), 1);
    QCOMPARE(chosen.count(), 0);

    panel.present(QStringLiteral("feature"), QStringLiteral("main"), QPoint(0, 0));
    panel.dismiss();
    QCOMPARE(dismissed.count(), 2);
    QCOMPARE(chosen.count(), 0);
}

void BadgeFlightTest::theListAsksForTheOperationTheReaderClicked() {
    GraphOperationPanel panel;
    QSignalSpy chosen(&panel, &GraphOperationPanel::chosen);
    QSignalSpy dismissed(&panel, &GraphOperationPanel::dismissed);

    panel.present(QStringLiteral("feature"), QStringLiteral("main"), QPoint(0, 0));
    const QList<QPushButton*> buttons = panel.findChildren<QPushButton*>();
    QVERIFY(!buttons.isEmpty());
    buttons.first()->click();   // the first entry is Rebase

    QCOMPARE(chosen.count(), 1);
    QCOMPARE(chosen.first().at(0).value<RefOperation>(), RefOperation::Rebase);
    // A choice is not also a dismissal: one answer, and the badge can land once.
    QCOMPARE(dismissed.count(), 0);
}

void BadgeFlightTest::theInteractiveEntryIsLiveAndReportsItself() {
    GraphOperationPanel panel;
    QSignalSpy chosen(&panel, &GraphOperationPanel::chosen);
    panel.present(QStringLiteral("feature"), QStringLiteral("main"), QPoint(0, 0));

    QPushButton* interactive = nullptr;
    for (QPushButton* button : panel.findChildren<QPushButton*>()) {
        if (button->text().startsWith(QStringLiteral("Interactive rebase"))) {
            interactive = button;
            break;
        }
    }
    QVERIFY(interactive);
    QVERIFY(interactive->isEnabled());
    interactive->click();
    QCOMPARE(chosen.count(), 1);
    QCOMPARE(chosen.first().at(0).value<RefOperation>(), RefOperation::InteractiveRebase);
}

QTEST_MAIN(BadgeFlightTest)
#include "test_badge_flight.moc"
