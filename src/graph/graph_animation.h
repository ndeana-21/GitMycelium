#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace gk {

/// One row of the graph at one moment — all the animator needs to diff.
struct GraphRowKey {
    std::string oid;
    int row = 0;
    int lane = 0;
};

/// What the graph is doing to one commit (or one label) between two snapshots.
enum class GraphMotion {
    Appear,   ///< a commit the model did not have a moment ago
    Vanish,   ///< a commit that left (dropped, hidden branch, working tree clean)
    Move,     ///< it exists in both, but its row or lane changed
    Pulse,    ///< HEAD moved here (a checkout, a reset, a commit)
    LabelIn,
    LabelOut,
};

/// One transition, stamped with the moment it started.
struct GraphTransition {
    GraphMotion motion = GraphMotion::Appear;
    std::string oid;
    std::string label;
    int fromRow = 0;
    int toRow = 0;
    int fromLane = 0;
    int toLane = 0;
    std::int64_t startMs = 0;
    std::int64_t durationMs = 0;
};

/// A deterministic, Qt-free description of the graph's motion.
///
/// It holds no pixels and no widgets: it is told what the graph looked like
/// before and after a change, and answers what is moving, fading or pulsing for
/// the next few hundred milliseconds. That is what makes it testable with a fake
/// clock — the painting reads it, but the timing logic does not need a screen.
class GraphAnimation {
public:
    static constexpr std::int64_t kFadeMs = 180;
    static constexpr std::int64_t kMoveMs = 260;
    static constexpr std::int64_t kPulseMs = 420;
    static constexpr std::int64_t kLabelMs = 160;

    /// Folds a new snapshot in, computing the transitions from the previous one.
    /// The first call only records a baseline: a freshly opened graph must not
    /// fade in every row at once.
    void observe(const std::vector<GraphRowKey>& rows, const std::set<std::string>& labels,
                 const std::string& headOid, std::int64_t nowMs);

    /// Records a snapshot without producing transitions. Used when new rows are
    /// only paged in on scroll: they are not a change the reader caused, and
    /// fading every page would turn scrolling into a light show.
    void baseline(const std::vector<GraphRowKey>& rows, const std::set<std::string>& labels,
                  const std::string& headOid);

    /// Forgets everything, including the baseline; the next `observe` starts fresh.
    void clear();

    bool advance(std::int64_t nowMs);
    bool empty() const { return m_transitions.empty(); }
    bool active(std::int64_t nowMs) const;

    const std::vector<GraphTransition>& transitions() const { return m_transitions; }

    /// Interpolation of a transition, clamped to [0, 1].
    static double progress(const GraphTransition& t, std::int64_t nowMs);

    /// The transition of a kind for a commit, or nullptr. Linear in the (small)
    /// number of transitions held for that commit.
    const GraphTransition* find(const std::string& oid, GraphMotion kind) const;
    const GraphTransition* findLabel(const std::string& label, GraphMotion kind) const;

private:
    void rebuildIndex();

    std::vector<GraphTransition> m_transitions;
    std::unordered_map<std::string, std::vector<int>> m_byOid;
    std::unordered_map<std::string, std::vector<int>> m_byLabel;
    std::vector<GraphRowKey> m_rows;
    std::set<std::string> m_labels;
    std::string m_head;
    bool m_hasBaseline = false;
};

} // namespace gk
