#include "ui/ref_tree_view.h"

#include "ui/drop_point.h"

#include "model/ref_model.h"
#include "theme/icons.h"
#include "theme/theme.h"
#include "ui/refs_delegate.h"

#include <QApplication>
#include <QDrag>
#include <QHelpEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QToolTip>

namespace gk {
namespace {

class EyeDelegate : public QStyledItemDelegate {
public:
    explicit EyeDelegate(RefTreeView* view) : QStyledItemDelegate(view), m_view(view) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const int eye = index.data(RefModel::EyeRole).toInt();
        if (eye == RefModel::EyeNone) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);
        const QRect eyeArea = m_view->eyeRect(index);
        opt.rect.setRight(std::min(opt.rect.right(), eyeArea.left() - 2));
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        if (m_view->eyeVisible(index)) {
            const bool hidden = eye == RefModel::EyeHidden;
            const int side = eyeArea.height();
            icon(hidden ? Icon::EyeOff : Icon::Eye, theme::textDim(), side)
                .paint(painter, eyeArea);
        }
    }

private:
    RefTreeView* m_view;
};

} // namespace

RefTreeView::RefTreeView(QWidget* parent) : QTreeView(parent) {
    setItemDelegate(new EyeDelegate(this));
    setMouseTracking(true);
}

QRect RefTreeView::eyeRect(const QModelIndex& index) const {
    const QRect row = visualRect(index);
    const int side = std::min(theme::iconSize(), row.height());
    const int right = viewport()->width() - 4;
    return QRect(right - side, row.top() + (row.height() - side) / 2, side, side);
}

bool RefTreeView::eyeVisible(const QModelIndex& index) const {
    const int eye = index.data(RefModel::EyeRole).toInt();
    if (eye == RefModel::EyeHidden) {
        return true;
    }
    return eye == RefModel::EyeShown && m_hoveredRow.isValid() &&
           QModelIndex(m_hoveredRow) == index.siblingAtColumn(0);
}

bool RefTreeView::onEye(const QPoint& pos, QModelIndex* index) const {
    const QModelIndex at = indexAt(pos);
    if (!at.isValid() || at.data(RefModel::EyeRole).toInt() == RefModel::EyeNone ||
        !eyeRect(at).adjusted(-3, 0, 3, 0).contains(pos)) {
        return false;
    }
    if (index) {
        *index = at.siblingAtColumn(0);
    }
    return true;
}

void RefTreeView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (onEye(event->position().toPoint())) {
        event->accept();
        return;
    }
    QTreeView::mouseDoubleClickEvent(event);
}

bool RefTreeView::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        QModelIndex index;
        if (onEye(help->pos(), &index)) {
            const bool hidden = index.data(RefModel::EyeRole).toInt() == RefModel::EyeHidden;
            QToolTip::showText(help->globalPos(),
                               hidden ? QStringLiteral("Show in the graph")
                                      : QStringLiteral("Hide from the graph"),
                               viewport(), eyeRect(index));
            return true;
        }
    }
    return QTreeView::viewportEvent(event);
}

void RefTreeView::dropEvent(QDropEvent* event) {
    rememberDropPoint(viewport()->mapToGlobal(event->position().toPoint()));
    QTreeView::dropEvent(event);
}

void RefTreeView::setColorResolver(std::function<int(const QString&)> resolver) {
    m_colorResolver = std::move(resolver);
}

void RefTreeView::mousePressEvent(QMouseEvent* event) {
    QModelIndex eyeIndex;
    if (event->button() == Qt::LeftButton && onEye(event->position().toPoint(), &eyeIndex)) {
        event->accept();
        emit eyeClicked(eyeIndex);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const QModelIndex index = indexAt(event->position().toPoint());
        const QString name = index.isValid() ? RefModel::refShortName(index) : QString();
        if (!name.isEmpty()) {
            emit refFocused(name);
        }
    }
    QTreeView::mousePressEvent(event);
}

void RefTreeView::mouseMoveEvent(QMouseEvent* event) {
    const QModelIndex index = indexAt(event->position().toPoint());
    const QPersistentModelIndex row(index.siblingAtColumn(0));
    if (row != m_hoveredRow) {
        m_hoveredRow = row;
        viewport()->update();
    }
    const QString name = index.isValid() ? RefModel::refShortName(index) : QString();
    if (m_hoveredRef != name) {
        m_hoveredRef = name;
        emit refHovered(name);
    }
    QTreeView::mouseMoveEvent(event);
}

void RefTreeView::leaveEvent(QEvent* event) {
    if (m_hoveredRow.isValid()) {
        m_hoveredRow = QPersistentModelIndex();
        viewport()->update();
    }
    if (!m_hoveredRef.isEmpty()) {
        m_hoveredRef.clear();
        emit refHovered(QString());
    }
    QTreeView::leaveEvent(event);
}

void RefTreeView::startDrag(Qt::DropActions supportedActions) {
    const QModelIndex index = currentIndex();
    if (!index.isValid()) {
        QTreeView::startDrag(supportedActions);
        return;
    }

    const QString name = RefModel::refShortName(index);
    if (name.isEmpty()) {
        return;
    }

    auto* data = RefModel::mimeDataForIndex(index);
    if (!data) {
        return;
    }

    const int colorIndex = m_colorResolver ? m_colorResolver(name) : 0;
    const QPixmap chip = RefsDelegate::dragPixmap(name, colorIndex, RefModel::refKind(index));

    auto* drag = new QDrag(this);
    drag->setMimeData(data);
    drag->setPixmap(chip);
    const QSizeF logical = chip.deviceIndependentSize();
    drag->setHotSpot(QPoint(static_cast<int>(logical.width() / 2.0),
                            static_cast<int>(logical.height() / 2.0)));
    drag->exec(Qt::MoveAction);
}

} // namespace gk
