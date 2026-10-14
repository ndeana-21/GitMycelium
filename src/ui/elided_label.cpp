#include "ui/elided_label.h"

#include <QFontMetrics>

namespace gk {

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void ElidedLabel::setFullText(const QString& text) {
    m_fullText = text;
    setToolTip(text);
    updateGeometry();
    elide();
}

QSize ElidedLabel::minimumSizeHint() const {
    const QFontMetrics metrics(font());
    return {metrics.horizontalAdvance(QStringLiteral("…")) * 4, metrics.height()};
}

QSize ElidedLabel::sizeHint() const {
    const QFontMetrics metrics(font());
    return {metrics.horizontalAdvance(m_fullText) + 2, metrics.height()};
}

void ElidedLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    elide();
}

void ElidedLabel::elide() {
    setText(QFontMetrics(font()).elidedText(m_fullText, Qt::ElideMiddle, width()));
}

} // namespace gk
