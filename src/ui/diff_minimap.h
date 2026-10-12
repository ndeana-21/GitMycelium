#pragma once

#include <QVector>
#include <QWidget>

class QMouseEvent;

namespace gk {

/// A slim overview of a diff, beside the viewer: one tick per changed line, and a
/// translucent band for the part currently on screen. Clicking or dragging scrolls the
/// viewer to that place.
///
/// It holds no document; the viewer hands it the marks and the viewport band, and it
/// reports where the reader scrubbed. That keeps the painting and the diff model apart.
class DiffMinimap : public QWidget {
    Q_OBJECT

public:
    explicit DiffMinimap(QWidget* parent = nullptr);

    /// One entry per rendered diff line: true where the line is a change.
    void setMarks(const QVector<bool>& changed);
    /// The visible band, as fractions of the whole: `start` from the top, `size` its
    /// length. Both in 0..1.
    void setViewport(qreal start, qreal size);
    int markCount() const { return m_marks.size(); }

    QSize sizeHint() const override;

signals:
    /// The reader scrubbed to this fraction (0 at the top, 1 at the bottom).
    void scrubbed(qreal fraction);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    void report(const QMouseEvent* event);

    QVector<bool> m_marks;
    qreal m_start = 0.0;
    qreal m_size = 1.0;
};

} // namespace gk
