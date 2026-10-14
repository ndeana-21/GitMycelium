#include "ui/flow_layout.h"

#include <QWidget>

#include <algorithm>
#include <utility>
#include <vector>

namespace gk {

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent), m_spacing(spacing) {
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() {
    while (QLayoutItem* item = takeAt(0)) {
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem* item) { m_items.append(item); }

int FlowLayout::count() const { return static_cast<int>(m_items.size()); }

QLayoutItem* FlowLayout::itemAt(int index) const { return m_items.value(index); }

QLayoutItem* FlowLayout::takeAt(int index) {
    return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const { return {}; }

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const { return arrange(QRect(0, 0, width, 0), false); }

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : m_items) {
        if (!item->isEmpty()) {
            size = size.expandedTo(item->minimumSize());
        }
    }
    const QMargins margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

QSize FlowLayout::sizeHint() const {
    int width = 0;
    int height = 0;
    for (const QLayoutItem* item : m_items) {
        if (item->isEmpty()) {
            continue;
        }
        width += (width > 0 ? m_spacing : 0) + item->sizeHint().width();
        height = std::max(height, item->sizeHint().height());
    }
    const QMargins margins = contentsMargins();
    return {width + margins.left() + margins.right(), height + margins.top() + margins.bottom()};
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::arrange(const QRect& rect, bool apply) const {
    const QRect area = rect.marginsRemoved(contentsMargins());
    int y = area.y();
    std::vector<std::pair<QLayoutItem*, QSize>> line;
    int lineWidth = 0;
    int lineHeight = 0;
    // A line is placed once it is full, so its items can be centred on its height: a
    // check box beside buttons sits on their middle, not on their top edge.
    const auto place = [&] {
        if (apply) {
            int x = area.x();
            for (const auto& [item, hint] : line) {
                item->setGeometry(QRect(QPoint(x, y + (lineHeight - hint.height()) / 2),
                                        QSize(std::min(hint.width(), area.width()), hint.height())));
                x += hint.width() + m_spacing;
            }
        }
        y += lineHeight + m_spacing;
        line.clear();
        lineWidth = 0;
        lineHeight = 0;
    };
    for (QLayoutItem* item : m_items) {
        if (item->isEmpty()) {
            continue;
        }
        const QSize hint = item->sizeHint();
        if (!line.empty() && lineWidth + m_spacing + hint.width() > area.width()) {
            place();
        }
        lineWidth += (line.empty() ? 0 : m_spacing) + hint.width();
        lineHeight = std::max(lineHeight, hint.height());
        line.emplace_back(item, hint);
    }
    if (!line.empty()) {
        place();
    }
    const int used = y - m_spacing - area.y();
    const QMargins margins = contentsMargins();
    return std::max(0, used) + margins.top() + margins.bottom();
}

} // namespace gk
