#include "ui/diff_minimap.h"

#include "theme/theme.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace gk {

DiffMinimap::DiffMinimap(QWidget* parent) : QWidget(parent) {
    setFixedWidth(12);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Overview of the changes"));
}

QSize DiffMinimap::sizeHint() const {
    return {12, 200};
}

void DiffMinimap::setMarks(const QVector<bool>& changed) {
    if (changed == m_marks) {
        return;
    }
    m_marks = changed;
    update();
}

void DiffMinimap::setViewport(qreal start, qreal size) {
    const qreal clampedStart = std::clamp(start, 0.0, 1.0);
    const qreal clampedSize = std::clamp(size, 0.0, 1.0);
    if (qFuzzyCompare(clampedStart, m_start) && qFuzzyCompare(clampedSize, m_size)) {
        return;
    }
    m_start = clampedStart;
    m_size = clampedSize;
    update();
}

void DiffMinimap::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), theme::surfaceAlt());

    const int height = std::max(1, this->height());
    const int count = m_marks.size();
    if (count > 0) {
        painter.setPen(theme::accent());
        for (int i = 0; i < count; ++i) {
            if (!m_marks.at(i)) {
                continue;
            }
            const int y = i * height / count;
            painter.drawLine(1, y, width() - 2, y);
        }
    }

    QColor band = theme::selection();
    band.setAlpha(80);
    const int top = static_cast<int>(m_start * height);
    const int bandHeight = std::max(3, static_cast<int>(m_size * height));
    painter.fillRect(QRect(0, top, width(), bandHeight), band);
}

void DiffMinimap::report(const QMouseEvent* event) {
    const int height = std::max(1, this->height());
    const qreal fraction = std::clamp(static_cast<qreal>(event->position().y()) / height, 0.0, 1.0);
    emit scrubbed(fraction);
}

void DiffMinimap::mousePressEvent(QMouseEvent* event) {
    report(event);
}

void DiffMinimap::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        report(event);
    }
}

} // namespace gk
