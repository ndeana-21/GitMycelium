#pragma once

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

    /// Injected so the rail and the graph agree on colours without the rail having to
    /// reach into the commit model.
    void setColorResolver(std::function<int(const QString&)> resolver);

signals:
    /// A reference was clicked, i.e. focused, but not checked out.
    void refFocused(QString shortName);
    /// Empty when the pointer left every row.
    void refHovered(QString shortName);

protected:
    /// The drop itself is handled by the model; record where it landed here, so the
    /// menu it leads to opens under the reader's hand rather than where the drag began.
    void dropEvent(QDropEvent* event) override;

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    std::function<int(const QString&)> m_colorResolver;
    QString m_hoveredRef;
};

} // namespace gk
