#pragma once

#include <QLabel>

namespace gk {

/// A one-line label that asks for its whole text but makes do with less, eliding the
/// middle. A plain QLabel's minimum width is its text, so a long path pushed the
/// window wider every time a file opened.
class ElidedLabel : public QLabel {
    Q_OBJECT

public:
    explicit ElidedLabel(QWidget* parent = nullptr);

    void setFullText(const QString& text);
    QString fullText() const { return m_fullText; }

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void elide();

    QString m_fullText;
};

} // namespace gk
