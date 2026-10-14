#include "git/topo_walk.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace gk {
namespace {

constexpr std::uint32_t kMissing = 0xffffffffU;
constexpr std::uint8_t kSeen = 1;
constexpr std::uint8_t kInIndegreeQueue = 2;

/// git's `compare_commits_by_gen_then_commit_date`, and the insertion order for a
/// tie, as its priority queue breaks one.
template <typename Queued>
bool before(const Queued& a, const Queued& b) {
    if (a.generation != b.generation) {
        return a.generation > b.generation;
    }
    if (a.time != b.time) {
        return a.time > b.time;
    }
    return a.order < b.order;
}

template <typename Queued>
bool heapLess(const Queued& a, const Queued& b) {
    return before(b, a);
}

} // namespace

bool parseCommitHeader(const char* data, std::size_t size, WalkCommit& out) {
    out.parents.clear();
    out.time = 0;
    const auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    std::string_view rest(data, size);
    while (!rest.empty()) {
        const std::size_t eol = rest.find('\n');
        const std::string_view line = rest.substr(0, eol);
        if (line.empty()) {
            break;
        }
        if (line.rfind("parent ", 0) == 0) {
            const std::string_view hex = line.substr(7);
            if (hex.size() < 2 * CommitGraph::kHashSize) {
                return false;
            }
            std::array<unsigned char, CommitGraph::kHashSize> id{};
            for (std::size_t i = 0; i < id.size(); ++i) {
                const int hi = hexValue(hex[2 * i]);
                const int lo = hexValue(hex[2 * i + 1]);
                if (hi < 0 || lo < 0) {
                    return false;
                }
                id[i] = static_cast<unsigned char>(hi << 4 | lo);
            }
            out.parents.push_back(id);
        } else if (line.rfind("committer ", 0) == 0) {
            // "committer Name <email> 1600000000 +0000": the date follows the email.
            const std::size_t close = line.rfind('>');
            if (close != std::string_view::npos) {
                std::int64_t time = 0;
                for (std::size_t i = close + 2; i < line.size() && line[i] >= '0' && line[i] <= '9';
                     ++i) {
                    time = time * 10 + (line[i] - '0');
                }
                out.time = time;
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(eol + 1);
    }
    return true;
}

std::size_t TopoWalk::RawHash::operator()(const RawOid& id) const {
    std::size_t h;
    std::memcpy(&h, id.data(), sizeof(h));
    return h;
}

TopoWalk::TopoWalk(const CommitGraph* graph, Lookup lookup)
    : m_graph(graph), m_lookup(std::move(lookup)), m_graphSize(graph ? graph->size() : 0) {
    m_indegree.assign(m_graphSize, 0);
    m_flags.assign(m_graphSize, 0);
}

std::uint32_t TopoWalk::nodeFor(const RawOid& id) {
    if (m_graph) {
        const std::uint32_t pos = m_graph->find(id.data());
        if (pos != CommitGraph::npos) {
            return pos;
        }
    }
    if (const auto it = m_extraIndex.find(id); it != m_extraIndex.end()) {
        return it->second;
    }
    WalkCommit commit;
    if (!m_lookup(id, commit)) {
        return kMissing;
    }
    const auto node = static_cast<std::uint32_t>(m_graphSize + m_extras.size());
    Extra extra;
    extra.id = id;
    extra.time = commit.time;
    extra.parentOffset = static_cast<std::uint32_t>(m_rawParents.size());
    extra.parentCount = static_cast<std::uint16_t>(commit.parents.size());
    m_rawParents.insert(m_rawParents.end(), commit.parents.begin(), commit.parents.end());
    m_extras.push_back(extra);
    m_extraIndex.emplace(id, node);
    m_indegree.push_back(0);
    m_flags.push_back(0);
    return node;
}

std::uint64_t TopoWalk::generationOf(std::uint32_t node) const {
    return node < m_graphSize ? m_graph->generation(node) : kInfinity;
}

std::int64_t TopoWalk::timeOf(std::uint32_t node) const {
    return node < m_graphSize ? m_graph->commitTime(node) : m_extras[node - m_graphSize].time;
}

const std::vector<std::uint32_t>& TopoWalk::parentsOf(std::uint32_t node) {
    allParentsOf(node);
    if (m_parentScratch.size() > 1 &&
        std::find(m_firstParentOnly.begin(), m_firstParentOnly.end(), node) !=
            m_firstParentOnly.end()) {
        m_parentScratch.resize(1);
    }
    return m_parentScratch;
}

const std::vector<std::uint32_t>& TopoWalk::allParentsOf(std::uint32_t node) {
    if (node < m_graphSize) {
        m_graph->parents(node, m_parentScratch);
        return m_parentScratch;
    }
    // Resolved on first use, not when the commit is read: resolving a parent reads
    // it, and doing that eagerly would recurse down a history with no graph.
    const std::uint32_t index = node - m_graphSize;
    if (!m_extras[index].resolved) {
        const auto offset = static_cast<std::uint32_t>(m_resolvedParents.size());
        const std::uint32_t rawOffset = m_extras[index].parentOffset;
        const std::uint16_t rawCount = m_extras[index].parentCount;
        std::uint16_t count = 0;
        for (std::uint32_t i = 0; i < rawCount; ++i) {
            const RawOid parent = m_rawParents[rawOffset + i];
            const std::uint32_t resolved = nodeFor(parent);
            if (resolved != kMissing) {
                m_resolvedParents.push_back(resolved);
                ++count;
            }
        }
        m_extras[index].parentOffset = offset;
        m_extras[index].parentCount = count;
        m_extras[index].resolved = true;
    }
    const Extra& extra = m_extras[index];
    m_parentScratch.assign(m_resolvedParents.begin() + extra.parentOffset,
                           m_resolvedParents.begin() + extra.parentOffset + extra.parentCount);
    return m_parentScratch;
}

void TopoWalk::pushIndegree(std::uint32_t node) {
    if (m_flags[node] & kInIndegreeQueue) {
        return;
    }
    m_flags[node] |= kInIndegreeQueue;
    m_indegreeQueue.push_back(Queued{generationOf(node), timeOf(node), m_queueOrder++, node});
    std::push_heap(m_indegreeQueue.begin(), m_indegreeQueue.end(), heapLess<Queued>);
}

void TopoWalk::computeIndegreesToDepth(std::uint64_t cutoff) {
    while (!m_indegreeQueue.empty() && m_indegreeQueue.front().generation >= cutoff) {
        std::pop_heap(m_indegreeQueue.begin(), m_indegreeQueue.end(), heapLess<Queued>);
        const std::uint32_t node = m_indegreeQueue.back().node;
        m_indegreeQueue.pop_back();
        for (const std::uint32_t parent : parentsOf(node)) {
            // git keeps "no children seen" as 1 so that 0 can mean "not counted yet".
            std::int32_t& count = m_indegree[parent];
            count = count != 0 ? count + 1 : 2;
            pushIndegree(parent);
        }
    }
}

void TopoWalk::start(const std::vector<RawOid>& tips,
                     const std::vector<RawOid>& firstParentOnly) {
    for (const RawOid& id : firstParentOnly) {
        if (const std::uint32_t node = nodeFor(id); node != kMissing) {
            m_firstParentOnly.push_back(node);
        }
    }
    std::vector<std::uint32_t> starts;
    starts.reserve(tips.size());
    for (const RawOid& tip : tips) {
        const std::uint32_t node = nodeFor(tip);
        if (node == kMissing || (m_flags[node] & kSeen)) {
            continue;
        }
        m_flags[node] |= kSeen;
        starts.push_back(node);
    }
    std::stable_sort(starts.begin(), starts.end(), [this](std::uint32_t a, std::uint32_t b) {
        return timeOf(a) > timeOf(b);
    });

    for (const std::uint32_t node : starts) {
        pushIndegree(node);
        m_minGeneration = std::min(m_minGeneration, generationOf(node));
        m_indegree[node] = 1;
    }
    computeIndegreesToDepth(m_minGeneration);

    for (const std::uint32_t node : starts) {
        if (m_indegree[node] == 1) {
            m_topo.push_back(node);
        }
    }
    // The topo queue is a stack, and the tips must come off it newest first.
    std::reverse(m_topo.begin(), m_topo.end());
}

void TopoWalk::expand(std::uint32_t node) {
    // A copy: the depth search below reads other commits' parents into the same
    // scratch buffer this list may live in.
    const std::vector<std::uint32_t> parents = parentsOf(node);
    for (const std::uint32_t parent : parents) {
        const std::uint64_t generation = generationOf(parent);
        if (generation < m_minGeneration) {
            m_minGeneration = generation;
            computeIndegreesToDepth(m_minGeneration);
        }
        if (--m_indegree[parent] == 1) {
            m_topo.push_back(parent);
        }
    }
}

bool TopoWalk::next(RawOid& id) {
    if (m_topo.empty()) {
        return false;
    }
    const std::uint32_t node = m_topo.back();
    m_topo.pop_back();
    m_indegree[node] = 0;
    expand(node);
    if (node < m_graphSize) {
        std::memcpy(id.data(), m_graph->oidAt(node), id.size());
    } else {
        id = m_extras[node - m_graphSize].id;
    }
    return true;
}

} // namespace gk
