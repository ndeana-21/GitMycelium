#pragma once

#include "core/types.h"

#include <QString>
#include <QStringList>

#include <deque>

namespace gk {

struct OplogEntry {
    QString label;
    RefState before;
    RefState after;
};

/// Per-repository operation log, so the window can put an operation back (undo)
/// or re-apply it (redo).
///
/// It records pointers, not file contents — see `RefState`. Only *completed*
/// operations are recorded: one that stops on a conflict is remembered as a
/// pending start and written when it finally finishes, so an abort records nothing.
class Oplog {
public:
    static constexpr std::size_t kMaxEntries = 100;

    void clear();

    /// A labelled operation is beginning. `beforeClean` says whether the
    /// repository was free of an open merge/rebase/cherry-pick; a moment already
    /// inside one leaves the earlier start in place.
    void noteStart(const RefState& before, bool beforeClean);

    /// The operation ended. `afterClean` false means it stopped on a conflict, so
    /// the pending start is kept for its continue or abort.
    void noteEnd(const QString& label, const RefState& after, bool afterClean);

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    QString undoLabel() const { return m_undo.empty() ? QString() : m_undo.back().label; }
    QString redoLabel() const { return m_redo.empty() ? QString() : m_redo.back().label; }

    const OplogEntry* undoTop() const { return m_undo.empty() ? nullptr : &m_undo.back(); }
    const OplogEntry* redoTop() const { return m_redo.empty() ? nullptr : &m_redo.back(); }

    void commitUndo();
    void commitRedo();

    QStringList describe() const;

private:
    void push(const QString& label, const RefState& before, const RefState& after);

    std::deque<OplogEntry> m_undo;
    std::deque<OplogEntry> m_redo;
    bool m_pending = false;
    RefState m_pendingBefore;
};

} // namespace gk
