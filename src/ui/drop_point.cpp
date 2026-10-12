#include "ui/drop_point.h"

namespace gk {
namespace {

QPoint g_lastDropPoint;

} // namespace

void rememberDropPoint(const QPoint& globalPos) {
    g_lastDropPoint = globalPos;
}

QPoint lastDropPoint() {
    return g_lastDropPoint;
}

} // namespace gk
