#include "ui/ref_tree_view.h"

#include "ui/drop_point.h"

#include "model/ref_model.h"
#include "ui/refs_delegate.h"

#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>

namespace gk {

RefTreeView::RefTreeView(QWidget* parent) : QTreeView(parent) {}

void RefTreeView::dropEvent(QDropEvent* event) {
    rememberDropPoint(viewport()->mapToGlobal(event->position().toPoint()));
    QTreeView::dropEvent(event);
}

void RefTreeView::setColorResolver(std::function<int(const QString&)> resolver) {
    m_colorResolver = std::move(resolver);
}

void RefTreeView::mousePressEvent(QMouseEvent* event) {
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
    const QString name = index.isValid() ? RefModel::refShortName(index) : QString();
    if (m_hoveredRef != name) {
        m_hoveredRef = name;
        emit refHovered(name);
    }
    QTreeView::mouseMoveEvent(event);
}

void RefTreeView::leaveEvent(QEvent* event) {
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
        // A group header is not a reference and must not be draggable.
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
