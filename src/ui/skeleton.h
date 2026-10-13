#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

namespace gk {

/// Placeholder bars drawn over a view whose data is still being read, in the shape
/// of what will appear: list rows, graph rows (a node and a line), or one line of
/// text. It covers its target and follows its size; while it is up, the mouse goes
/// to it, not to the stale content underneath.
class Skeleton : public QWidget {
    Q_OBJECT

public:
    enum class Shape { List, Graph, Line };

    /// Covers `target` (a view's viewport, or a label). Hidden until `setActive(true)`.
    Skeleton(QWidget* target, Shape shape);

    void setActive(bool active);
    bool isActive() const { return m_active; }

    /// A headless run must not depend on the clock: the bars stay still.
    static void setAnimationsEnabled(bool on);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void follow();
    void paintRow(QPainter& painter, int top, int height, int index);

    QWidget* m_target;
    Shape m_shape;
    bool m_active = false;
    QTimer m_frame;
    QElapsedTimer m_clock;
};

} // namespace gk
