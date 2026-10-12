#include "core/porcelain_status.h"

#include <algorithm>
#include <string>

namespace gk {
namespace {

ChangeKind kindOf(char code) {
    switch (code) {
    case 'A': return ChangeKind::Added;
    case 'D': return ChangeKind::Deleted;
    case 'R': return ChangeKind::Renamed;
    case 'T': return ChangeKind::TypeChange;
    // A copy is only reported when copy detection is on; without it the same path
    // would read as an addition, which is what the old side of a copy is.
    case 'C': return ChangeKind::Added;
    default: return ChangeKind::Modified;
    }
}

/// The path after `fields` space-separated fields. A path may itself contain
/// spaces, so it is everything that follows, never a field of its own.
std::string_view pathAfterFields(std::string_view record, int fields) {
    std::size_t at = 0;
    for (int i = 0; i < fields; ++i) {
        at = record.find(' ', at);
        if (at == std::string_view::npos) {
            return {};
        }
        ++at;
    }
    return record.substr(at);
}

void addSides(std::vector<FileChange>& out, std::string_view xy, std::string_view path) {
    if (xy.size() < 2 || path.empty()) {
        return;
    }
    if (xy[0] != '.') {
        FileChange change;
        change.path = std::string(path);
        change.kind = kindOf(xy[0]);
        change.staged = true;
        out.push_back(std::move(change));
    }
    if (xy[1] != '.') {
        FileChange change;
        change.path = std::string(path);
        change.kind = kindOf(xy[1]);
        out.push_back(std::move(change));
    }
}

} // namespace

std::vector<FileChange> parsePorcelainStatus(std::string_view text) {
    std::vector<FileChange> out;
    std::size_t at = 0;
    const auto nextRecord = [&text, &at]() -> std::string_view {
        if (at >= text.size()) {
            return {};
        }
        const std::size_t end = text.find('\0', at);
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        const std::string_view record = text.substr(at, stop - at);
        at = stop + 1;
        return record;
    };

    while (at < text.size()) {
        const std::string_view record = nextRecord();
        if (record.size() < 2) {
            continue;
        }
        const std::string_view xy = record.size() >= 4 ? record.substr(2, 2) : std::string_view{};
        switch (record[0]) {
        case '1':
            // 1 XY sub mH mI mW hH hI path
            addSides(out, xy, pathAfterFields(record, 8));
            break;
        case '2':
            // 2 XY sub mH mI mW hH hI Xscore path, then the original path as a
            // record of its own.
            addSides(out, xy, pathAfterFields(record, 9));
            nextRecord();
            break;
        case 'u': {
            // u XY sub m1 m2 m3 mW h1 h2 h3 path
            FileChange change;
            change.path = std::string(pathAfterFields(record, 10));
            change.kind = ChangeKind::Conflicted;
            out.push_back(std::move(change));
            break;
        }
        case '?': {
            FileChange change;
            change.path = std::string(record.substr(2));
            change.kind = ChangeKind::Untracked;
            out.push_back(std::move(change));
            break;
        }
        default:
            break;
        }
    }
    // git lists the tracked changes first and the untracked after; a path sorts
    // them back together, keeping a file's staged side before its unstaged one.
    std::stable_sort(out.begin(), out.end(),
                     [](const FileChange& a, const FileChange& b) { return a.path < b.path; });
    return out;
}

} // namespace gk
