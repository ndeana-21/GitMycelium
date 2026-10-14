#include "model/ref_model.h"
#include "model/ref_pins.h"

#include <QDir>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

RefInfo branch(const QString& name, RefKind kind = RefKind::LocalBranch) {
    RefInfo ref;
    ref.fullName = (kind == RefKind::Tag ? "refs/tags/" : "refs/heads/") + name.toStdString();
    ref.shortName = name.toStdString();
    ref.target = "0123456789abcdef";
    ref.kind = kind;
    return ref;
}

void collect(const QStandardItemModel& model, const QModelIndex& parent, const QString& trail,
             QStringList* out) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        const QString label = index.data(Qt::DisplayRole).toString();
        const QString path = trail.isEmpty() ? label : trail + QLatin1Char('/') + label;
        out->push_back(path);
        collect(model, index, path, out);
    }
}

QStringList shape(const RefModel& model) {
    QStringList out;
    collect(model, QModelIndex(), QString(), &out);
    return out;
}

} // namespace

class RefTreeTest : public QObject {
    Q_OBJECT

private slots:
    void namesSplitIntoFolders();
    void singleChildFolderChainsMerge();
    void filterKeepsOnlyMatchingBranches();
    void filterUnderstandsRegex();
    void brokenRegexHidesEverythingAndSaysWhy();
    void pinnedBranchMovesOutOfItsSection();
    void pinnedFolderTakesItsSubtree();
    void pinnedOrderIsTheOrderTheyWerePinned();
    void pinsSurviveAReloadAndStayPerRepository();
    void pinsAreAdoptedFromTheOldCheckoutPathKey();
};

void RefTreeTest::namesSplitIntoFolders() {
    RefModel model;
    model.rebuild({branch("feature/login"), branch("feature/ui"), branch("main")});

    const QStringList rows = shape(model);
    QVERIFY2(rows.contains(QStringLiteral("Local/feature")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(rows.contains(QStringLiteral("Local/feature/login")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(rows.contains(QStringLiteral("Local/feature/ui")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(rows.contains(QStringLiteral("Local/main")), qPrintable(rows.join(QLatin1Char(' '))));

    QVERIFY(!rows.contains(QStringLiteral("Local/feature/feature/login")));
}

void RefTreeTest::singleChildFolderChainsMerge() {
    RefModel model;
    // Three levels, each holding nothing but the next: one row, not three.
    model.rebuild({branch("origin/feature/wip", RefKind::RemoteBranch)});

    const QStringList rows = shape(model);
    QCOMPARE(rows, QStringList({QStringLiteral("Remote"), QStringLiteral("Remote/origin/feature"),
                                QStringLiteral("Remote/origin/feature/wip")}));
}

void RefTreeTest::filterKeepsOnlyMatchingBranches() {
    RefModel model;
    model.rebuild({branch("feature/login"), branch("feature/ui"), branch("main"),
                   branch("origin/main", RefKind::RemoteBranch)});

    model.setFilter(QStringLiteral("LOGIN"), false); // case does not matter

    const QStringList rows = shape(model);
    QVERIFY2(rows.contains(QStringLiteral("Local/feature/login")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(!rows.contains(QStringLiteral("Local/feature/ui")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(!rows.contains(QStringLiteral("Local/main")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(!rows.contains(QStringLiteral("Remote")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY(model.filterError().isEmpty());

    model.setFilter(QString(), false);
    QCOMPARE(shape(model).size(), 8);
}

void RefTreeTest::filterUnderstandsRegex() {
    RefModel model;
    model.rebuild({branch("feature/login"), branch("feature/ui"), branch("main")});

    model.setFilter(QStringLiteral("^feature/(login|ui)$"), true);

    const QStringList rows = shape(model);
    QVERIFY2(rows.contains(QStringLiteral("Local/feature/login")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(rows.contains(QStringLiteral("Local/feature/ui")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(!rows.contains(QStringLiteral("Local/main")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY(model.filterError().isEmpty());

    model.setFilter(QStringLiteral("^feature/(login|ui)$"), false);
    QCOMPARE(shape(model).size(), 0);
}

void RefTreeTest::brokenRegexHidesEverythingAndSaysWhy() {
    RefModel model;
    model.rebuild({branch("main"), branch("feature/login")});

    model.setFilter(QStringLiteral("(["), true);

    QCOMPARE(shape(model).size(), 0);
    QVERIFY(!model.filterError().isEmpty());
}

void RefTreeTest::pinnedBranchMovesOutOfItsSection() {
    RefModel model;
    model.rebuild({branch("feature/login"), branch("feature/ui"), branch("main")});

    model.setPinned({QStringLiteral("feature/login")});

    const QStringList rows = shape(model);
    QCOMPARE(rows.at(0), QStringLiteral("Pinned"));
    QCOMPARE(rows.at(1), QStringLiteral("Pinned/feature/login"));
    QVERIFY2(!rows.contains(QStringLiteral("Local/feature/login")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(rows.contains(QStringLiteral("Local/feature/ui")), qPrintable(rows.join(QLatin1Char(' '))));
}

void RefTreeTest::pinnedFolderTakesItsSubtree() {
    RefModel model;
    model.rebuild({branch("feature/login"), branch("feature/ui"), branch("main")});

    model.setPinned({QStringLiteral("feature/")});

    const QStringList rows = shape(model);
    QVERIFY2(rows.contains(QStringLiteral("Pinned/feature/login")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(rows.contains(QStringLiteral("Pinned/feature/ui")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(!rows.contains(QStringLiteral("Local/feature")), qPrintable(rows.join(QLatin1Char(' '))));
    QVERIFY2(rows.contains(QStringLiteral("Local/main")), qPrintable(rows.join(QLatin1Char(' '))));
}

void RefTreeTest::pinnedOrderIsTheOrderTheyWerePinned() {
    RefModel model;
    model.rebuild({branch("alpha"), branch("beta"), branch("gamma")});

    model.setPinned({QStringLiteral("gamma"), QStringLiteral("alpha")});

    const QStringList rows = shape(model);
    QCOMPARE(rows.at(0), QStringLiteral("Pinned"));
    QCOMPARE(rows.at(1), QStringLiteral("Pinned/gamma"));
    QCOMPARE(rows.at(2), QStringLiteral("Pinned/alpha"));
}

void RefTreeTest::pinsSurviveAReloadAndStayPerRepository() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString store = dir.filePath(QStringLiteral("pins.json"));

    {
        RefPins pins(store);
        pins.load(QStringLiteral("/repos/one"));
        pins.setPinned(QStringLiteral("main"), true);
        pins.setPinned(QStringLiteral("feature/"), true);
        QVERIFY(pins.isPinned(QStringLiteral("main")));
        QVERIFY(pins.isPinned(QStringLiteral("feature/")));
        QVERIFY(!pins.isPinned(QStringLiteral("feature"))); // the branch, not the folder

        pins.load(QStringLiteral("/repos/two"));
        QVERIFY(pins.names().isEmpty()); // another repository's pins are not ours
    }

    RefPins reloaded(store);
    reloaded.load(QStringLiteral("/repos/one"));
    QCOMPARE(reloaded.names().size(), 2);
    QCOMPARE(reloaded.names().at(0), QStringLiteral("main"));
    QCOMPARE(reloaded.names().at(1), QStringLiteral("feature/"));

    reloaded.setPinned(QStringLiteral("main"), false);
    QCOMPARE(reloaded.names().size(), 1);

    RefPins second(store);
    second.load(QStringLiteral("/repos/two"));
    QVERIFY(second.names().isEmpty());

    RefPins third(store);
    third.load(QStringLiteral("/repos/one"));
    QCOMPARE(third.names().size(), 1); // the removal was written, not just held
}

void RefTreeTest::pinsAreAdoptedFromTheOldCheckoutPathKey() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString store = dir.filePath(QStringLiteral("pins.json"));

    // Pins used to be keyed by the checkout path. Opening the repository through
    // the new common-git-dir key must find them, not read as "no pins".
    {
        RefPins old(store);
        old.load(QStringLiteral("/checkouts/project"));
        old.setPinned(QStringLiteral("release"), true);
    }

    RefPins migrated(store);
    migrated.load(QStringLiteral("/checkouts/project/.git"),
                  QStringLiteral("/checkouts/project"));
    QCOMPARE(migrated.names(), QStringList{QStringLiteral("release")});

    RefPins fresh(store);
    fresh.load(QStringLiteral("/checkouts/project/.git"));
    QCOMPARE(fresh.names(), QStringList{QStringLiteral("release")});

    RefPins stale(store);
    stale.load(QStringLiteral("/checkouts/project"));
    QVERIFY(stale.names().isEmpty());
}

QTEST_MAIN(RefTreeTest)
#include "test_ref_tree.moc"
