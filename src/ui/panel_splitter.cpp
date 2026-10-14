#include "ui/panel_splitter.h"

#include "theme/theme.h"

#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSplitterHandle>

#include <algorithm>
#include <numeric>

namespace gk {
namespace {

constexpr int kHandleWidth = 12;
constexpr int kButtonHeight = 36;

/// A hairline with a fold button halfway down. Handle `i` sits before widget `i`, so
/// handle 1 belongs to the first panel and the last handle to the last one.
class PanelHandle : public QSplitterHandle {
public:
    PanelHandle(Qt::Orientation orientation, PanelSplitter* parent)
        : QSplitterHandle(orientation, parent), m_splitter(parent) {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor line = m_hoverLine ? theme::accent() : theme::border();
        painter.fillRect(QRectF(width() / 2.0 - 0.5, 0, 1.0, height()), line);

        const int panel = panelIndex();
        if (panel < 0) {
            return;
        }
        const QRectF button = buttonRect();
        painter.setPen(QPen(m_hoverButton ? theme::accent() : theme::borderStrong(), 1.0));
        painter.setBrush(m_hoverButton ? theme::surfaceAlt() : theme::panel());
        painter.drawRoundedRect(button.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);

        const bool collapsed = m_splitter->isPanelCollapsed(panel);
        const bool first = panel == 0;
        const bool pointsLeft = first != collapsed;
        const QPointF centre = button.center();
        const qreal reach = 2.5;
        const qreal half = 4.0;
        QPainterPath arrow;
        arrow.moveTo(centre.x() + (pointsLeft ? reach : -reach), centre.y() - half);
        arrow.lineTo(centre.x() + (pointsLeft ? -reach : reach), centre.y());
        arrow.lineTo(centre.x() + (pointsLeft ? reach : -reach), centre.y() + half);
        QPen pen(m_hoverButton ? theme::accent() : theme::textDim(), 1.6);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(arrow);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (panelIndex() >= 0 && buttonRect().contains(event->position())) {
            m_pressedButton = true;
            event->accept();
            return;
        }
        QSplitterHandle::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (m_pressedButton) {
            m_pressedButton = false;
            if (buttonRect().contains(event->position())) {
                m_splitter->togglePanel(panelIndex());
            }
            event->accept();
            return;
        }
        QSplitterHandle::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const bool onButton = panelIndex() >= 0 && buttonRect().contains(event->position());
        if (onButton != m_hoverButton) {
            m_hoverButton = onButton;
            setCursor(onButton ? Qt::PointingHandCursor : Qt::SplitHCursor);
            update();
        }
        if (m_pressedButton) {
            event->accept();
            return;
        }
        QSplitterHandle::mouseMoveEvent(event);
    }

    void enterEvent(QEnterEvent* event) override {
        QSplitterHandle::enterEvent(event);
        m_hoverLine = true;
        update();
    }

    void leaveEvent(QEvent* event) override {
        QSplitterHandle::leaveEvent(event);
        m_hoverLine = false;
        m_hoverButton = false;
        update();
    }

private:
    int panelIndex() const {
        const int handle = m_splitter->indexOf(const_cast<PanelHandle*>(this));
        if (handle == 1) {
            return 0;
        }
        if (handle == m_splitter->count() - 1 && handle > 1) {
            return handle;
        }
        return -1;
    }

    QRectF buttonRect() const {
        const qreal h = std::min<qreal>(kButtonHeight, height());
        return QRectF(0, (height() - h) / 2.0, width(), h);
    }

    PanelSplitter* m_splitter;
    bool m_hoverLine = false;
    bool m_hoverButton = false;
    bool m_pressedButton = false;
};

} // namespace

PanelSplitter::PanelSplitter(QWidget* parent) : QSplitter(Qt::Horizontal, parent) {
    setHandleWidth(kHandleWidth);
    connect(this, &QSplitter::splitterMoved, this, [this] { noteSizes(); });
}

QSplitterHandle* PanelSplitter::createHandle() {
    return new PanelHandle(orientation(), this);
}

bool PanelSplitter::isSidePanel(int index) const {
    return count() >= 2 && (index == 0 || index == count() - 1);
}

bool PanelSplitter::isPanelCollapsed(int index) const {
    return isSidePanel(index) && index < static_cast<int>(m_collapsed.size()) &&
           m_collapsed[static_cast<std::size_t>(index)];
}

void PanelSplitter::togglePanel(int index) {
    setPanelCollapsed(index, !isPanelCollapsed(index));
}

void PanelSplitter::setPanelCollapsed(int index, bool collapsed) {
    if (!isSidePanel(index) || isPanelCollapsed(index) == collapsed) {
        return;
    }
    QList<int> current = sizes();
    m_restoreSizes.resize(static_cast<std::size_t>(count()), 0);
    const int middle = index == 0 ? 1 : index - 1;
    if (collapsed) {
        m_restoreSizes[static_cast<std::size_t>(index)] = current.at(index);
        current[middle] += current.at(index);
        current[index] = 0;
    } else {
        QWidget* panel = widget(index);
        int wanted = m_restoreSizes[static_cast<std::size_t>(index)];
        if (wanted <= 0) {
            wanted = std::max(panel->sizeHint().width(), panel->minimumSizeHint().width());
        }
        wanted = std::max(wanted, panel->minimumSizeHint().width());
        const int middleMinimum = widget(middle)->minimumSizeHint().width();
        wanted = std::min(wanted, std::max(0, current.at(middle) - middleMinimum));
        if (wanted <= 0) {
            wanted = std::max(1, panel->minimumSizeHint().width());
        }
        current[middle] = std::max(0, current.at(middle) - wanted);
        current[index] = wanted;
    }
    setSizes(current);
    m_collapsed.resize(static_cast<std::size_t>(count()), false);
    m_collapsed[static_cast<std::size_t>(index)] = collapsed;
    if (QSplitterHandle* h = handle(index == 0 ? 1 : index)) {
        h->update();
    }
    emit panelCollapsedChanged(index, collapsed);
}

void PanelSplitter::noteSizes() {
    const QList<int> current = sizes();
    m_collapsed.resize(static_cast<std::size_t>(count()), false);
    for (int i = 0; i < count(); ++i) {
        if (!isSidePanel(i)) {
            continue;
        }
        const bool collapsed = i < current.size() && current.at(i) == 0;
        if (collapsed != m_collapsed[static_cast<std::size_t>(i)]) {
            m_collapsed[static_cast<std::size_t>(i)] = collapsed;
            emit panelCollapsedChanged(i, collapsed);
        }
        if (QSplitterHandle* h = handle(i == 0 ? 1 : i)) {
            h->update();
        }
    }
}

} // namespace gk
