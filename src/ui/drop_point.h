#pragma once

#include <QPoint>

namespace gk {

/// Records where the last drop landed, in global coordinates.
///
/// A drop menu has to open where the drop happened. The obvious way to ask is
/// QCursor::pos(), and it is wrong: a drag owns the cursor, and while one is in
/// flight the reported position can stay at the point the drag started — which
/// puts the menu under the reader's starting point instead of under their hand.
/// The drop event itself knows, so the views record it as they receive it and the
/// menu asks for it here.
void rememberDropPoint(const QPoint& globalPos);

/// The last recorded drop position, or a null point if no drop has happened yet.
QPoint lastDropPoint();

} // namespace gk
