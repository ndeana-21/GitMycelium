#pragma once

#include <QSplitter>

namespace gk {

/// A splitter whose handle is a hairline with a grip in the middle, so the gap
/// between two panels reads as something to drag.
///
/// The Commit Panel and the commit details both use it, and that is the point: two
/// panels stacked in the same column with two different-looking drag handles is the
/// kind of detail that reads as a bug rather than as a choice.
class GripSplitter : public QSplitter {
public:
    explicit GripSplitter(Qt::Orientation orientation, QWidget* parent = nullptr);

protected:
    QSplitterHandle* createHandle() override;
};

} // namespace gk
