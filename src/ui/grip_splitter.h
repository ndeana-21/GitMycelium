#pragma once

#include <QSplitter>

namespace gk {

class GripSplitter : public QSplitter {
public:
    explicit GripSplitter(Qt::Orientation orientation, QWidget* parent = nullptr);

protected:
    QSplitterHandle* createHandle() override;
};

} // namespace gk
