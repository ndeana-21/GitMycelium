#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gk {

/// A read-only view of a repository's commit-graph. Positions are global across the
/// split chain, numbered exactly as git numbers them.
class CommitGraph {
public:
    static constexpr std::uint32_t npos = 0xffffffffU;
    static constexpr std::size_t kHashSize = 20;

    /// The repository's graph, or null when it has none or cannot be trusted. Null
    /// means "walk without it", never an error.
    static std::unique_ptr<CommitGraph> open(const std::string& objectsDir);

    CommitGraph(const CommitGraph&) = delete;
    CommitGraph& operator=(const CommitGraph&) = delete;
    ~CommitGraph();

    std::uint32_t size() const { return m_total; }
    std::uint32_t find(const unsigned char* oid) const;
    const unsigned char* oidAt(std::uint32_t pos) const;
    /// Topological level: greater than every parent's.
    std::uint32_t generation(std::uint32_t pos) const;
    std::int64_t commitTime(std::uint32_t pos) const;
    void parents(std::uint32_t pos, std::vector<std::uint32_t>& out) const;

private:
    struct Layer {
        const unsigned char* data = nullptr;
        std::size_t length = 0;
        const unsigned char* fanout = nullptr;
        const unsigned char* oids = nullptr;
        const unsigned char* commitData = nullptr;
        const unsigned char* extraEdges = nullptr;
        std::size_t extraEdgeCount = 0;
        std::uint32_t count = 0;
        std::uint32_t first = 0;
    };

    CommitGraph() = default;
    bool addLayer(const std::string& path, std::uint32_t expectedBases);
    bool hasGenerations() const;
    const Layer& layerOf(std::uint32_t pos) const;

    std::vector<Layer> m_layers;
    std::uint32_t m_total = 0;
};

} // namespace gk
