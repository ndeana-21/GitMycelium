#pragma once

#include "git/commit_graph.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace gk {

struct WalkCommit {
    std::vector<std::array<unsigned char, CommitGraph::kHashSize>> parents;
    std::int64_t time = 0;
};

/// Reads the parents and the committer date out of a raw commit object.
bool parseCommitHeader(const char* data, std::size_t size, WalkCommit& out);

/// `git rev-list --topo-order`, one commit at a time.
///
/// libgit2's sorted revwalk parses the entire history before it returns the first
/// commit: on a million commits that is seconds and most of a gigabyte before the
/// graph shows a single row. This is git's own incremental algorithm instead:
/// child counts are computed only down to the shallowest generation the walk has
/// reached, and a commit is emitted the moment every child is out.
///
/// The order is git's, tie-breaks included, so `git rev-list --topo-order` is the
/// oracle the tests compare against.
class TopoWalk {
public:
    using RawOid = std::array<unsigned char, CommitGraph::kHashSize>;
    /// Reads a commit the graph does not hold. False when the object is missing:
    /// the walk then treats it as absent, the way git skips a missing parent.
    using Lookup = std::function<bool(const RawOid& id, WalkCommit& out)>;

    TopoWalk(const CommitGraph* graph, Lookup lookup);

    /// `firstParentOnly` are commits whose other parents are not history: a stash
    /// is a merge of HEAD with the index (and the untracked files), and those
    /// helper commits are not drawn.
    void start(const std::vector<RawOid>& tips, const std::vector<RawOid>& firstParentOnly = {});
    bool next(RawOid& id);

    /// Commits the graph did not hold, which the walk had to read one by one.
    std::size_t readOutsideGraph() const { return m_extras.size(); }

private:
    static constexpr std::uint64_t kInfinity = ~std::uint64_t{0};

    /// A commit outside the graph. Without a graph that is every commit, so it is
    /// kept small: its parents live in shared pools, read ones in `m_rawParents`
    /// and, once resolved to nodes, in `m_resolvedParents`.
    struct Extra {
        std::int64_t time = 0;
        RawOid id{};
        std::uint32_t parentOffset = 0;
        std::uint16_t parentCount = 0;
        bool resolved = false;
    };
    struct Queued {
        std::uint64_t generation;
        std::int64_t time;
        std::uint64_t order;
        std::uint32_t node;
    };
    struct RawHash {
        std::size_t operator()(const RawOid& id) const;
    };

    std::uint32_t nodeFor(const RawOid& id);
    std::uint64_t generationOf(std::uint32_t node) const;
    std::int64_t timeOf(std::uint32_t node) const;
    const std::vector<std::uint32_t>& parentsOf(std::uint32_t node);
    const std::vector<std::uint32_t>& allParentsOf(std::uint32_t node);
    void pushIndegree(std::uint32_t node);
    void computeIndegreesToDepth(std::uint64_t cutoff);
    void expand(std::uint32_t node);

    const CommitGraph* m_graph;
    Lookup m_lookup;
    std::uint32_t m_graphSize = 0;

    std::vector<Extra> m_extras;
    std::vector<RawOid> m_rawParents;
    std::vector<std::uint32_t> m_resolvedParents;
    std::unordered_map<RawOid, std::uint32_t, RawHash> m_extraIndex;

    std::vector<std::int32_t> m_indegree;
    std::vector<std::uint8_t> m_flags;
    std::vector<Queued> m_indegreeQueue;
    std::uint64_t m_queueOrder = 0;
    std::vector<std::uint32_t> m_topo;
    std::uint64_t m_minGeneration = kInfinity;
    std::vector<std::uint32_t> m_parentScratch;
    std::vector<std::uint32_t> m_firstParentOnly;
};

} // namespace gk
