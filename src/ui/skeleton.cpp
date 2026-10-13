#include "ui/skeleton.h"

#include "theme/theme.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace gk {
namespace {

bool g_animate = true;

constexpr int kFrameMs = 33;
constexpr qreal kSweepMs = 1400.0;

/// Widths that vary row to row the way names do, without randomness: the same
/// skeleton looks the same on every repaint.
qreal widthFraction(int index) {
    static constexpr qreal kWidths[] = {0.62, 0.48, 0.71, 0.55, 0.39, 0.66, 0.5, 0.58};
    return kWidths[index % (sizeof(kWidths) / sizeof(kWidths[0]))];
}

} // namespace

Skeleton::Skeleton(QWidget* target, Shape shape)
    : QWidget(target), m_target(target), m_shape(shape) {
    setObjectName(QStringLiteral("skeleton"));
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
    target->installEventFilter(this);
    m_frame.setInterval(kFrameMs);
    connect(&m_frame, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void Skeleton::setAnimationsEnabled(bool on) {
    g_animate = on;
}

void Skeleton::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (active) {
        follow();
        raise();
        show();
        m_clock.start();
        if (g_animate) {
            m_frame.start();
        }
    } else {
        m_frame.stop();
        hide();
    }
}

bool Skeleton::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_target && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        follow();
    }
    return QWidget::eventFilter(watched, event);
}

void Skeleton::follow() {
    setGeometry(m_target->rect());
}

void Skeleton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), m_shape == Shape::Line ? Qt::transparent : theme::background());

    if (m_shape == Shape::Line) {
        paintRow(painter, 0, height(), 0);
        return;
    }
    const int rowHeight = theme::rowHeight();
    for (int top = 0, index = 0; top < height(); top += rowHeight, ++index) {
        paintRow(painter, top, rowHeight, index);
    }
}

void Skeleton::paintRow(QPainter& painter, int top, int height, int index) {
    const QColor base = theme::surfaceAlt();
    QColor highlight = theme::borderStrong();
    // The sweep: a band of lighter colour crossing the bars left to right.
    qreal phase = -1.0;
    if (g_animate && m_clock.isValid()) {
        phase = std::fmod(static_cast<qreal>(m_clock.elapsed()), kSweepMs) / kSweepMs;
    }

    const int barHeight = std::max(6, height / 3);
    const int y = top + (height - barHeight) / 2;
    int left = 8;
    if (m_shape == Shape::Graph) {
        const int node = std::max(8, height / 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(base);
        painter.drawRect(QRectF(left + node / 2.0 - 1, top, 2, height));
        painter.drawEllipse(QRectF(left, top + (height - node) / 2.0, node, node));
        left += node + 12;
    }
    const int available = std::max(0, width() - left - 8);
    const qreal fraction = m_shape == Shape::Line ? 0.8 : widthFraction(index);
    const QRectF bar(left, y, available * fraction, barHeight);
    if (bar.width() <= 0) {
        return;
    }

    QBrush brush(base);
    if (phase >= 0.0) {
        QLinearGradient sweep(0, 0, width(), 0);
        const qreal centre = phase * 1.6 - 0.3;
        sweep.setColorAt(0.0, base);
        sweep.setColorAt(std::clamp(centre - 0.15, 0.0, 1.0), base);
        sweep.setColorAt(std::clamp(centre, 0.0, 1.0), highlight);
        sweep.setColorAt(std::clamp(centre + 0.15, 0.0, 1.0), base);
        sweep.setColorAt(1.0, base);
        brush = QBrush(sweep);
    }
    QPainterPath path;
    path.addRoundedRect(bar, barHeight / 2.0, barHeight / 2.0);
    painter.fillPath(path, brush);
}

} // namespace gk
