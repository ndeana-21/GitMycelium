#pragma once

#include "core/types.h"
#include "ui/ref_pill_layout.h"

#include <QString>
#include <QStyledItemDelegate>

#include <functional>

class QPixmap;
class QPainter;
class QFont;

namespace gk {

/// Draws one label pill. Shared by the delegate and the graph's hover overlay, so
/// the same label cannot be drawn two different ways.
void paintRefPill(QPainter* painter, const RefPill& pill, const QFont& font);

class RefsDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit RefsDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    /// A chip carrying the branch name, used as the drag image. Qt draws nothing while
    /// dragging without a pixmap, so the reader would drag an invisible thing.
    static QPixmap dragPixmap(const QString& name, int colorIndex, RefKind kind);

    void setLabelAlphaHook(std::function<double(const QString& name)> alpha);

private:
    std::function<double(const QString&)> m_labelAlpha;
};

} // namespace gk
