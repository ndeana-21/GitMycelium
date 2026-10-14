#pragma once

#include <QLayout>
#include <QList>

namespace gk {

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 6);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;

    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    int arrange(const QRect& rect, bool apply) const;

    QList<QLayoutItem*> m_items;
    int m_spacing;
};

} // namespace gk
