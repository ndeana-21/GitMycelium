#pragma once

#include <QSplitter>

#include <vector>

namespace gk {

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
