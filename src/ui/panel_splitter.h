#pragma once

#include <QSplitter>

#include <vector>

namespace gk {

/// The window's three columns: a side panel on each edge and the content between.
///
/// A side panel folds away and comes back from a button on its handle, and the
/// handle stays on screen while it is folded, with its arrow pointing at where the
/// panel is — dragging a panel against the window edge was the only way to hide it,
/// and nothing then said there was anything left to bring back.
class PanelSplitter : public QSplitter {
    Q_OBJECT

public:
    explicit PanelSplitter(QWidget* parent = nullptr);

    /// Only the first and the last widget are panels; anything else is ignored.
    void setPanelCollapsed(int index, bool collapsed);
    bool isPanelCollapsed(int index) const;
    void togglePanel(int index);

signals:
    void panelCollapsedChanged(int index, bool collapsed);

protected:
    QSplitterHandle* createHandle() override;

private:
    bool isSidePanel(int index) const;
    void noteSizes();

    std::vector<int> m_restoreSizes;
    std::vector<bool> m_collapsed;
};

} // namespace gk
