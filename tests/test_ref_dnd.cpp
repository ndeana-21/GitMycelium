#include "model/ref_model.h"
#include "ui/drop_point.h"
#include "ui/ref_tree_view.h"

#include <QApplication>
#include <QMimeData>
#include <QSignalSpy>
#include <QtTest>

using namespace gk;

namespace {

RefInfo branch(const std::string& shortName, const std::string& target) {
    RefInfo ref;
    ref.fullName = "refs/heads/" + shortName;
    ref.shortName = shortName;
    ref.kind = RefKind::LocalBranch;
    ref.target = target;
    return ref;
}

/// A view whose drop handling the test can invoke directly. Reaching the protected
/// override from outside needs a subclass, and sending a synthetic QDropEvent to the
/// viewport does not get routed into it.
class TestRefTreeView : public RefTreeView {
public:
    using RefTreeView::RefTreeView;
    void drop(QDropEvent* event) { dropEvent(event); }
};

} // namespace

class RefDndTest : public QObject {
    Q_OBJECT

private slots:
    void init();

    void encodesAndDecodesADraggedBranch();
    void groupHeadersAreNotDraggable();
    void dropOnAnotherBranchEmitsIntent();
    void dropOnAGroupOrItselfIsRefused();

    void aDropRemembersWhereItLanded();

private:
    QModelIndex indexOf(const QString& text) const;

    std::unique_ptr<RefModel> m_model;

};

void RefDndTest::init() {
    m_model = std::make_unique<RefModel>();
    m_model->setHeadBranch(QStringLiteral("main"));
    m_model->rebuild({branch("main", "aaaa"), branch("feature/login", "bbbb"),
                      branch("feature/ui", "cccc")});
    QVERIFY(m_model->rowCount() > 0);
}

QModelIndex RefDndTest::indexOf(const QString& text) const {
    // Sections are found by what they show; references by their real name,
    // because inside a folder a row shows only the tail of it.
    const int roles[] = {static_cast<int>(Qt::DisplayRole), static_cast<int>(RefModel::ShortNameRole)};
    for (const int role : roles) {
        const QModelIndexList found = m_model->match(m_model->index(0, 0), role, text, -1,
                                                     Qt::MatchExactly | Qt::MatchRecursive);
        if (!found.isEmpty()) {
            return found.first();
        }
    }
    return {};
}

void RefDndTest::encodesAndDecodesADraggedBranch() {
    const QModelIndex feature = indexOf(QStringLiteral("feature/login"));
    QVERIFY(feature.isValid());

    std::unique_ptr<QMimeData> data(m_model->mimeData({feature}));
    QVERIFY(data != nullptr);
    QVERIFY(data->hasFormat(QString::fromLatin1(kRefMimeType)));

    const RefModel::DraggedRef decoded = RefModel::decode(data.get());
    QCOMPARE(decoded.shortName, QStringLiteral("feature/login"));
    QCOMPARE(decoded.fullName, QStringLiteral("refs/heads/feature/login"));
    QCOMPARE(decoded.target, QStringLiteral("bbbb"));
    QCOMPARE(decoded.kind, RefKind::LocalBranch);

    QMimeData foreign;
    foreign.setText(QStringLiteral("just text"));
    QVERIFY(RefModel::decode(&foreign).shortName.isEmpty());
}

void RefDndTest::groupHeadersAreNotDraggable() {
    const QModelIndex group = indexOf(QStringLiteral("Local"));
    QVERIFY(group.isValid());
    QVERIFY(!group.parent().isValid());

    QVERIFY(m_model->mimeData({group}) == nullptr);
    const QModelIndex feature = indexOf(QStringLiteral("feature/ui"));
    std::unique_ptr<QMimeData> data(m_model->mimeData({feature}));
    QVERIFY(data != nullptr);
    QVERIFY(!m_model->canDropMimeData(data.get(), Qt::MoveAction, -1, -1, group));
}

void RefDndTest::dropOnAnotherBranchEmitsIntent() {
    const QModelIndex feature = indexOf(QStringLiteral("feature/login"));
    const QModelIndex main = indexOf(QStringLiteral("main"));
    QVERIFY(feature.isValid());
    QVERIFY(main.isValid());

    std::unique_ptr<QMimeData> data(m_model->mimeData({feature}));
    QVERIFY(data != nullptr);
    QVERIFY(m_model->canDropMimeData(data.get(), Qt::MoveAction, -1, -1, main));

    QSignalSpy spy(m_model.get(), &RefModel::refDroppedOnRef);
    QVERIFY(m_model->dropMimeData(data.get(), Qt::MoveAction, -1, -1, main));
    QCOMPARE(spy.count(), 1);

    const auto args = spy.takeFirst();
    const auto source = args.at(0).value<RefModel::DraggedRef>();
    QCOMPARE(source.shortName, QStringLiteral("feature/login"));
    QCOMPARE(args.at(1).toString(), QStringLiteral("main"));
}

void RefDndTest::dropOnAGroupOrItselfIsRefused() {
    const QModelIndex feature = indexOf(QStringLiteral("feature/ui"));
    std::unique_ptr<QMimeData> data(m_model->mimeData({feature}));
    QVERIFY(data != nullptr);

    QSignalSpy spy(m_model.get(), &RefModel::refDroppedOnRef);
    QVERIFY(!m_model->dropMimeData(data.get(), Qt::MoveAction, -1, -1, feature));
    QCOMPARE(spy.count(), 0);
}

/// The menu a drop leads to must open where the drop is. The cursor position is not
/// usable for that — during a drag it can still report where the drag began, which
/// puts the menu under the point the drag started from.
void RefDndTest::aDropRemembersWhereItLanded() {
    TestRefTreeView view;
    view.resize(400, 300);

    QMimeData mime;
    // Deliberately not the origin of anything: whatever is recorded has to be the
    // position carried by the drop event itself.
    const QPoint whereTheDropLanded(140, 90);
    QDropEvent event(QPointF(whereTheDropLanded), Qt::CopyAction, &mime, Qt::LeftButton,
                     Qt::NoModifier);
    view.drop(&event);

    QCOMPARE(lastDropPoint(), view.viewport()->mapToGlobal(whereTheDropLanded));
    QVERIFY(!lastDropPoint().isNull());
}

QTEST_MAIN(RefDndTest)
#include "test_ref_dnd.moc"
