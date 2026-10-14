#pragma once

#include <QString>
#include <QStyledItemDelegate>

#include <functional>

namespace gk {

/// Paints the commit-graph column. Everything is drawn per row, so painting is
/// O(visible rows) even on a million-commit history.
class GraphDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit GraphDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    static int laneWidth() { return 26; }
    /// Inset of the first lane from the cell's left edge. A fixed distance, not a
    /// share of the cell: lane 0 must not move when the column is resized.
    static int lanePadding() { return 16; }
    static int nodeRadius() { return 11; }

    static int widthForLanes(int lanes);

    /// Injected by the view: the opacity to fade a commit's graph with, and whether
    /// the animation overlay owns that row — then this delegate draws the background
    /// but not the graph, or the gliding snapshot and the new drawing would both show.
    void setAnimationHooks(std::function<double(const QString& oid)> fadeAlpha,
                           std::function<bool(const QString& oid)> isMorphing);

private:
    std::function<double(const QString&)> m_fadeAlpha;
    std::function<bool(const QString&)> m_isMorphing;
};

} // namespace gk
