#pragma once

#include <QVector>
#include <QWidget>

class QMouseEvent;

namespace gk {

class DiffMinimap : public QWidget {
    Q_OBJECT

public:
    explicit DiffMinimap(QWidget* parent = nullptr);

    void setMarks(const QVector<bool>& changed);
    /// The visible band, as fractions of the whole: `start` from the top, `size` its
    /// length. Both in 0..1.
    void setViewport(qreal start, qreal size);
    int markCount() const { return m_marks.size(); }

    QSize sizeHint() const override;

signals:
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
