#include "git/commit_graph.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <filesystem>
#include <memory>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gk {
namespace {

constexpr std::uint32_t kNoParent = 0x70000000U;
constexpr std::uint32_t kExtraEdgeFlag = 0x80000000U;
constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kChunkEntrySize = 12;
constexpr std::size_t kFanoutSize = 256 * 4;
constexpr std::size_t kCommitDataSize = CommitGraph::kHashSize + 16;

std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
           std::uint32_t{p[3]};
}

std::uint64_t be64(const unsigned char* p) {
    return (std::uint64_t{be32(p)} << 32) | be32(p + 4);
}

std::uint32_t chunkId(const char (&id)[5]) {
    return be32(reinterpret_cast<const unsigned char*>(id));
}

} // namespace

std::unique_ptr<CommitGraph> CommitGraph::open(const std::string& objectsDir) {
    std::unique_ptr<CommitGraph> graph(new CommitGraph());
    const std::string info = objectsDir + "/info";

    std::ifstream chain(info + "/commit-graphs/commit-graph-chain");
    if (chain) {
        std::string hash;
        std::uint32_t bases = 0;
        while (std::getline(chain, hash)) {
            if (hash.empty()) {
                continue;
            }
            if (!graph->addLayer(info + "/commit-graphs/graph-" + hash + ".graph", bases)) {
                return nullptr;
            }
            ++bases;
        }
    } else if (!graph->addLayer(info + "/commit-graph", 0)) {
        return nullptr;
    }
    if (graph->m_layers.empty() || !graph->hasGenerations()) {
        return nullptr;
    }
    return graph;
}

namespace {

#ifdef _WIN32
/// Read, not mapped: Windows refuses to replace a file while a view of it is mapped,
/// and `git commit-graph write` (ours or the reader's) renames over this one.
const unsigned char* mapFile(const std::string& path, std::size_t* length) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size < kHeaderSize) {
        return nullptr;
    }
    std::ifstream in(path, std::ios::binary);
    auto data = std::make_unique<unsigned char[]>(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(data.get()), static_cast<std::streamsize>(size))) {
        return nullptr;
    }
    *length = static_cast<std::size_t>(size);
    return data.release();
}

void unmapFile(const unsigned char* data, std::size_t) {
    delete[] data;
}
#else
const unsigned char* mapFile(const std::string& path, std::size_t* length) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kHeaderSize)) {
        ::close(fd);
        return nullptr;
    }
    *length = static_cast<std::size_t>(st.st_size);
    void* mapped = mmap(nullptr, *length, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    return mapped == MAP_FAILED ? nullptr : static_cast<const unsigned char*>(mapped);
}

void unmapFile(const unsigned char* data, std::size_t length) {
    munmap(const_cast<unsigned char*>(data), length);
}
#endif

} // namespace

CommitGraph::~CommitGraph() {
    for (const Layer& layer : m_layers) {
        unmapFile(layer.data, layer.length);
    }
}

bool CommitGraph::addLayer(const std::string& path, std::uint32_t expectedBases) {
    std::size_t length = 0;
    const unsigned char* mapped = mapFile(path, &length);
    if (!mapped) {
        return false;
    }

    Layer layer;
    layer.data = mapped;
    layer.length = length;
    // Registered before validation so the destructor unmaps a rejected layer too.
    m_layers.push_back(layer);
    Layer& added = m_layers.back();

    const unsigned char* d = added.data;
    if (std::memcmp(d, "CGPH", 4) != 0 || d[4] != 1 || d[5] != 1 || d[7] != expectedBases) {
        return false;
    }
    const std::size_t chunks = d[6];
    if (kHeaderSize + (chunks + 1) * kChunkEntrySize > length) {
        return false;
    }

    std::uint64_t edgeOffset = 0;
    std::uint64_t edgeEnd = 0;
    for (std::size_t i = 0; i < chunks; ++i) {
        const unsigned char* entry = d + kHeaderSize + i * kChunkEntrySize;
        const std::uint32_t id = be32(entry);
        const std::uint64_t offset = be64(entry + 4);
        const std::uint64_t end = be64(entry + kChunkEntrySize + 4);
        if (offset > length || end > length || end < offset) {
            return false;
        }
        if (id == chunkId("OIDF")) {
            if (end - offset != kFanoutSize) {
                return false;
            }
            added.fanout = d + offset;
        } else if (id == chunkId("OIDL")) {
            added.oids = d + offset;
        } else if (id == chunkId("CDAT")) {
            added.commitData = d + offset;
        } else if (id == chunkId("EDGE")) {
            edgeOffset = offset;
            edgeEnd = end;
        }
    }
    if (!added.fanout || !added.oids || !added.commitData) {
        return false;
    }
    added.count = be32(added.fanout + 255 * 4);
    const std::size_t oidBytes = std::size_t{added.count} * kHashSize;
    const std::size_t dataBytes = std::size_t{added.count} * kCommitDataSize;
    if (added.oids + oidBytes > d + length || added.commitData + dataBytes > d + length) {
        return false;
    }
    if (edgeEnd > edgeOffset) {
        added.extraEdges = d + edgeOffset;
        added.extraEdgeCount = static_cast<std::size_t>((edgeEnd - edgeOffset) / 4);
    }
    added.first = m_total;
    m_total += added.count;
    return true;
}

bool CommitGraph::hasGenerations() const {
    // A layer written without generation numbers stores zeros, which would make a
    // child look no deeper than its parents and end the walk's depth search early.
    for (std::uint32_t pos = 0; pos < m_total; ++pos) {
        if (generation(pos) == 0) {
            return false;
        }
    }
    return true;
}

const CommitGraph::Layer& CommitGraph::layerOf(std::uint32_t pos) const {
    for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
        if (pos >= it->first) {
            return *it;
        }
    }
    return m_layers.front();
}

std::uint32_t CommitGraph::find(const unsigned char* oid) const {
    for (const Layer& layer : m_layers) {
        const std::uint32_t lo = oid[0] == 0 ? 0 : be32(layer.fanout + (oid[0] - 1) * 4);
        const std::uint32_t hi = be32(layer.fanout + oid[0] * 4);
        std::uint32_t left = lo;
        std::uint32_t right = std::min(hi, layer.count);
        while (left < right) {
            const std::uint32_t mid = left + (right - left) / 2;
            const int cmp = std::memcmp(layer.oids + std::size_t{mid} * kHashSize, oid, kHashSize);
            if (cmp == 0) {
                return layer.first + mid;
            }
            if (cmp < 0) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
    }
    return npos;
}

const unsigned char* CommitGraph::oidAt(std::uint32_t pos) const {
    const Layer& layer = layerOf(pos);
    return layer.oids + std::size_t{pos - layer.first} * kHashSize;
}

std::uint32_t CommitGraph::generation(std::uint32_t pos) const {
    const Layer& layer = layerOf(pos);
    const unsigned char* data =
        layer.commitData + std::size_t{pos - layer.first} * kCommitDataSize + kHashSize + 8;
    return be32(data) >> 2;
}

std::int64_t CommitGraph::commitTime(std::uint32_t pos) const {
    const Layer& layer = layerOf(pos);
    const unsigned char* data =
        layer.commitData + std::size_t{pos - layer.first} * kCommitDataSize + kHashSize + 8;
    return static_cast<std::int64_t>((std::uint64_t{be32(data) & 0x3U} << 32) | be32(data + 4));
}

void CommitGraph::parents(std::uint32_t pos, std::vector<std::uint32_t>& out) const {
    out.clear();
    const Layer& layer = layerOf(pos);
    const unsigned char* data =
        layer.commitData + std::size_t{pos - layer.first} * kCommitDataSize + kHashSize;
    const auto accept = [this, &out](std::uint32_t parent) {
        if (parent < m_total) {
            out.push_back(parent);
        }
    };

    const std::uint32_t first = be32(data);
    if (first == kNoParent) {
        return;
    }
    accept(first);
    const std::uint32_t second = be32(data + 4);
    if (second == kNoParent) {
        return;
    }
    if ((second & kExtraEdgeFlag) == 0) {
        accept(second);
        return;
    }
    // An octopus: the rest of the parents run through the extra-edge list until
    // the entry whose top bit marks it as the last.
    for (std::size_t i = second & ~kExtraEdgeFlag; i < layer.extraEdgeCount; ++i) {
        const std::uint32_t edge = be32(layer.extraEdges + i * 4);
        accept(edge & ~kExtraEdgeFlag);
        if (edge & kExtraEdgeFlag) {
            break;
        }
    }
}

} // namespace gk
