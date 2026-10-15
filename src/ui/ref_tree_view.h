#pragma once

#include <QPersistentModelIndex>
#include <QTreeView>

#include <functional>

namespace gk {

/// The reference rail on the left. Supplies the drag image a plain QTreeView lacks
/// (Qt only draws one when the QDrag is given a pixmap), using the same chip the
/// graph's label column draws.
class RefTreeView : public QTreeView {
    Q_OBJECT

public:
    explicit RefTreeView(QWidget* parent = nullptr);

    void setColorResolver(std::function<int(const QString&)> resolver);

    /// The eye at the right edge of a branch, folder or section row: closed while
    /// hidden from the graph, open only under the mouse.
    QRect eyeRect(const QModelIndex& index) const;
    bool eyeVisible(const QModelIndex& index) const;

signals:
    void refFocused(QString shortName);
    void refHovered(QString shortName);
    void eyeClicked(const QModelIndex& index);

protected:
    /// The drop itself is handled by the model; record where it landed here, so the
    /// menu it leads to opens under the reader's hand rather than where the drag began.
    void dropEvent(QDropEvent* event) override;

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    std::function<int(const QString&)> m_colorResolver;
    bool onEye(const QPoint& pos, QModelIndex* index = nullptr) const;

    QString m_hoveredRef;
    QPersistentModelIndex m_hoveredRow;
};

} // namespace gk
