#include "ui/grip_splitter.h"

#include "theme/theme.h"

#include <QEnterEvent>
#include <QPainter>
#include <QSplitterHandle>

namespace gk {
namespace {

class GripHandle : public QSplitterHandle {
public:
    using QSplitterHandle::QSplitterHandle;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor line = underMouse() ? theme::accent() : theme::border();
        painter.fillRect(QRectF(0, height() / 2.0 - 0.5, width(), 1.0), line);
        const QRectF grip(width() / 2.0 - 16, height() / 2.0 - 2, 32, 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(underMouse() ? theme::accent() : theme::borderStrong());
        painter.drawRoundedRect(grip, 2, 2);
    }

    void enterEvent(QEnterEvent* event) override {
        QSplitterHandle::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent* event) override {
        QSplitterHandle::leaveEvent(event);
        update();
    }
};

} // namespace

GripSplitter::GripSplitter(Qt::Orientation orientation, QWidget* parent)
    : QSplitter(orientation, parent) {
    setChildrenCollapsible(false);
    setHandleWidth(9);
}

QSplitterHandle* GripSplitter::createHandle() {
    return new GripHandle(orientation(), this);
}

} // namespace gk
