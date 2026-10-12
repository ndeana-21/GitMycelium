#include "graph/graph_animation.h"

#include <algorithm>

namespace gk {

void GraphAnimation::observe(const std::vector<GraphRowKey>& rows,
                             const std::set<std::string>& labels, const std::string& headOid,
                             std::int64_t nowMs) {
    if (!m_hasBaseline) {
        // The first snapshot is the starting point, not a change from nothing: an
        // opened repository must not fade every row in.
        m_rows = rows;
        m_labels = labels;
        m_head = headOid;
        m_hasBaseline = true;
        return;
    }

    const auto push = [&](GraphMotion motion, const std::string& oid, const std::string& label,
                          int fromRow, int fromLane, int toRow, int toLane,
                          std::int64_t duration) {
        GraphTransition t;
        t.motion = motion;
        t.oid = oid;
        t.label = label;
        t.fromRow = fromRow;
        t.fromLane = fromLane;
        t.toRow = toRow;
        t.toLane = toLane;
        t.startMs = nowMs;
        t.durationMs = duration;
        m_transitions.push_back(std::move(t));
    };

    std::unordered_map<std::string, const GraphRowKey*> before;
    before.reserve(m_rows.size());
    for (const GraphRowKey& row : m_rows) {
        before[row.oid] = &row;
    }

    std::unordered_map<std::string, bool> present;
    present.reserve(rows.size());
    for (const GraphRowKey& row : rows) {
        present[row.oid] = true;
        const auto it = before.find(row.oid);
        if (it == before.end()) {
            push(GraphMotion::Appear, row.oid, {}, row.row, row.lane, row.row, row.lane, kFadeMs);
        } else if (it->second->row != row.row || it->second->lane != row.lane) {
            push(GraphMotion::Move, row.oid, {}, it->second->row, it->second->lane, row.row,
                 row.lane, kMoveMs);
        }
    }
    for (const GraphRowKey& row : m_rows) {
        if (!present.count(row.oid)) {
            push(GraphMotion::Vanish, row.oid, {}, row.row, row.lane, row.row, row.lane, kFadeMs);
        }
    }

    if (!headOid.empty() && headOid != m_head) {
        for (const GraphRowKey& row : rows) {
            if (row.oid == headOid) {
                push(GraphMotion::Pulse, row.oid, {}, row.row, row.lane, row.row, row.lane,
                     kPulseMs);
                break;
            }
        }
    }

    for (const std::string& label : labels) {
        if (!m_labels.count(label)) {
            push(GraphMotion::LabelIn, {}, label, 0, 0, 0, 0, kLabelMs);
        }
    }
    for (const std::string& label : m_labels) {
        if (!labels.count(label)) {
            push(GraphMotion::LabelOut, {}, label, 0, 0, 0, 0, kLabelMs);
        }
    }

    m_rows = rows;
    m_labels = labels;
    m_head = headOid;
    rebuildIndex();
}

void GraphAnimation::baseline(const std::vector<GraphRowKey>& rows,
                              const std::set<std::string>& labels, const std::string& headOid) {
    m_rows = rows;
    m_labels = labels;
    m_head = headOid;
    m_hasBaseline = true;
}

void GraphAnimation::clear() {
    m_transitions.clear();
    m_byOid.clear();
    m_byLabel.clear();
    m_rows.clear();
    m_labels.clear();
    m_head.clear();
    m_hasBaseline = false;
}

bool GraphAnimation::advance(std::int64_t nowMs) {
    if (m_transitions.empty()) {
        return false;
    }
    m_transitions.erase(
        std::remove_if(m_transitions.begin(), m_transitions.end(),
                       [nowMs](const GraphTransition& t) {
                           return nowMs - t.startMs >= t.durationMs;
                       }),
        m_transitions.end());
    rebuildIndex();
    return !m_transitions.empty();
}

bool GraphAnimation::active(std::int64_t nowMs) const {
    for (const GraphTransition& t : m_transitions) {
        if (nowMs - t.startMs < t.durationMs) {
            return true;
        }
    }
    return false;
}

double GraphAnimation::progress(const GraphTransition& t, std::int64_t nowMs) {
    if (t.durationMs <= 0) {
        return 1.0;
    }
    const double p = static_cast<double>(nowMs - t.startMs) / static_cast<double>(t.durationMs);
    return std::clamp(p, 0.0, 1.0);
}

void GraphAnimation::rebuildIndex() {
    m_byOid.clear();
    m_byLabel.clear();
    for (std::size_t i = 0; i < m_transitions.size(); ++i) {
        const GraphTransition& t = m_transitions[i];
        if (!t.oid.empty()) {
            m_byOid[t.oid].push_back(static_cast<int>(i));
        }
        if (!t.label.empty()) {
            m_byLabel[t.label].push_back(static_cast<int>(i));
        }
    }
}

const GraphTransition* GraphAnimation::find(const std::string& oid, GraphMotion kind) const {
    const auto it = m_byOid.find(oid);
    if (it == m_byOid.end()) {
        return nullptr;
    }
    for (int index : it->second) {
        if (m_transitions[static_cast<std::size_t>(index)].motion == kind) {
            return &m_transitions[static_cast<std::size_t>(index)];
        }
    }
    return nullptr;
}

const GraphTransition* GraphAnimation::findLabel(const std::string& label,
                                                 GraphMotion kind) const {
    const auto it = m_byLabel.find(label);
    if (it == m_byLabel.end()) {
        return nullptr;
    }
    for (int index : it->second) {
        if (m_transitions[static_cast<std::size_t>(index)].motion == kind) {
            return &m_transitions[static_cast<std::size_t>(index)];
        }
    }
    return nullptr;
}

} // namespace gk
