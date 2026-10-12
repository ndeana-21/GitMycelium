#include "repo/oplog.h"

namespace gk {

void Oplog::clear() {
    m_undo.clear();
    m_redo.clear();
    m_pending = false;
    m_pendingBefore = RefState{};
}

void Oplog::noteStart(const RefState& before, bool beforeClean) {
    if (!beforeClean) {
        // Already inside an operation: its start is the one worth remembering,
        // and it is already held. A continue or an abort arrives here.
        return;
    }
    m_pendingBefore = before;
    m_pending = true;
}

void Oplog::noteEnd(const QString& label, const RefState& after, bool afterClean) {
    if (!m_pending) {
        return;
    }
    if (!afterClean) {
        // Stopped on a conflict. The entry is written when the operation is
        // carried to its end, so an undo never lands in the middle of one.
        return;
    }
    const RefState before = m_pendingBefore;
    m_pending = false;
    push(label, before, after);
}

void Oplog::push(const QString& label, const RefState& before, const RefState& after) {
    if (before == after) {
        // Staging a file that was already staged, an up-to-date merge, a rebase
        // that replays nothing: the pointers did not move, so there is nothing to
        // put back.
        return;
    }
    m_undo.push_back(OplogEntry{label, before, after});
    if (m_undo.size() > kMaxEntries) {
        m_undo.pop_front();
    }
    // A new operation makes the redo future unreachable, exactly as in any editor.
    m_redo.clear();
}

void Oplog::commitUndo() {
    if (m_undo.empty()) {
        return;
    }
    m_redo.push_back(m_undo.back());
    m_undo.pop_back();
}

void Oplog::commitRedo() {
    if (m_redo.empty()) {
        return;
    }
    m_undo.push_back(m_redo.back());
    m_redo.pop_back();
}

QStringList Oplog::describe() const {
    QStringList lines;
    lines << QStringLiteral("undo stack (newest first):");
    if (m_undo.empty()) {
        lines << QStringLiteral("  (empty)");
    }
    for (auto it = m_undo.rbegin(); it != m_undo.rend(); ++it) {
        lines << QStringLiteral("  %1").arg(it->label);
    }
    lines << QStringLiteral("redo stack (newest first):");
    if (m_redo.empty()) {
        lines << QStringLiteral("  (empty)");
    }
    for (auto it = m_redo.rbegin(); it != m_redo.rend(); ++it) {
        lines << QStringLiteral("  %1").arg(it->label);
    }
    return lines;
}

} // namespace gk
