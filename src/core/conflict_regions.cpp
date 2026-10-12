#include "core/conflict_regions.h"

#include <algorithm>

namespace gk {
namespace {

/// Splits into lines, each keeping its own line ending. The text is written back
/// byte for byte where nothing was decided, so the line endings cannot be
/// normalised out from under the file.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start + 1));
        start = end + 1;
    }
    return lines;
}

std::size_t markerRun(std::string_view line, char marker) {
    std::size_t run = 0;
    while (run < line.size() && line[run] == marker) {
        ++run;
    }
    if (run < kDefaultConflictMarkerSize) {
        return 0;
    }
    if (run == line.size()) {
        return run;
    }
    const char next = line[run];
    return next == ' ' || next == '\t' || next == '\r' || next == '\n' ? run : 0;
}

void flushPlain(std::vector<ConflictPiece>* pieces, std::vector<std::string>* pending) {
    if (pending->empty()) {
        return;
    }
    ConflictPiece piece;
    piece.isRegion = false;
    piece.lines = *pending;
    pieces->push_back(std::move(piece));
    pending->clear();
}

} // namespace

std::size_t conflictOpeningSize(std::string_view line) {
    return markerRun(line, '<');
}

ConflictMarker conflictMarkerOf(std::string_view line, std::size_t size) {
    for (const auto& [marker, kind] : {std::pair{'<', ConflictMarker::Ours},
                                       std::pair{'|', ConflictMarker::Base},
                                       std::pair{'=', ConflictMarker::Split},
                                       std::pair{'>', ConflictMarker::Theirs}}) {
        if (markerRun(line, marker) == size) {
            return kind;
        }
    }
    return ConflictMarker::None;
}

std::size_t safeConflictMarkerSize(const std::vector<std::string_view>& texts) {
    std::size_t longest = 0;
    for (const std::string_view text : texts) {
        std::size_t start = 0;
        while (start < text.size()) {
            const char first = text[start];
            if (first == '<' || first == '|' || first == '=' || first == '>') {
                std::size_t run = start;
                while (run < text.size() && text[run] == first) {
                    ++run;
                }
                longest = std::max(longest, run - start);
            }
            const std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
    }
    return std::max(kDefaultConflictMarkerSize, longest + 1);
}

bool ConflictDocument::balanced() const {
    for (const ConflictRegion& region : regions) {
        if (!region.balanced) {
            return false;
        }
    }
    return true;
}

ConflictDocument parseConflictDocument(const std::string& marked) {
    ConflictDocument doc;
    const std::vector<std::string> lines = splitLines(marked);
    std::vector<std::string> pending;

    std::size_t i = 0;
    while (i < lines.size()) {
        const std::size_t size = conflictOpeningSize(lines[i]);
        if (size == 0) {
            pending.push_back(lines[i]);
            ++i;
            continue;
        }
        const auto is = [&](std::size_t line, ConflictMarker kind) {
            return line < lines.size() && conflictMarkerOf(lines[line], size) == kind;
        };

        flushPlain(&doc.pieces, &pending);

        const std::size_t blockStart = i;
        ConflictRegion region;
        region.firstLine = blockStart;

        ++i;
        while (i < lines.size() && !is(i, ConflictMarker::Base) && !is(i, ConflictMarker::Split)) {
            region.ours.push_back(lines[i]);
            ++i;
        }
        if (is(i, ConflictMarker::Base)) {
            region.hasBase = true;
            ++i;
            while (i < lines.size() && !is(i, ConflictMarker::Split)) {
                region.base.push_back(lines[i]);
                ++i;
            }
        }
        if (is(i, ConflictMarker::Split)) {
            ++i;
            while (i < lines.size() && !is(i, ConflictMarker::Theirs)) {
                region.theirs.push_back(lines[i]);
                ++i;
            }
        }
        if (is(i, ConflictMarker::Theirs)) {
            region.balanced = true;
            ++i;
        } else {
            // Unbalanced markers: this is where the file runs out. The region is
            // still reported so the editor can show it, but it cannot be settled.
            region.balanced = false;
        }

        region.lineCount = i - blockStart;
        region.raw.assign(lines.begin() + static_cast<std::ptrdiff_t>(blockStart),
                          lines.begin() + static_cast<std::ptrdiff_t>(i));
        const std::size_t index = doc.regions.size();
        doc.regions.push_back(std::move(region));

        ConflictPiece piece;
        piece.isRegion = true;
        piece.region = index;
        doc.pieces.push_back(std::move(piece));
    }

    flushPlain(&doc.pieces, &pending);
    return doc;
}

void RegionLineTake::resize(const ConflictRegion& region) {
    ours.assign(region.ours.size(), 0);
    theirs.assign(region.theirs.size(), 0);
}

bool RegionLineTake::anyTaken() const {
    for (const char taken : ours) {
        if (taken) {
            return true;
        }
    }
    for (const char taken : theirs) {
        if (taken) {
            return true;
        }
    }
    return false;
}

bool RegionLineTake::isTaken(bool oursSide, std::size_t line) const {
    const std::vector<char>& side = oursSide ? ours : theirs;
    return line < side.size() && side[line] != 0;
}

void RegionLineTake::toggle(bool oursSide, std::size_t line) {
    std::vector<char>& side = oursSide ? ours : theirs;
    if (line < side.size()) {
        side[line] = side[line] ? 0 : 1;
    }
}

void RegionLineTake::takeAll(bool oursSide, bool take) {
    std::vector<char>& side = oursSide ? ours : theirs;
    std::fill(side.begin(), side.end(), take ? 1 : 0);
}

std::vector<std::string> regionChosenLines(const ConflictRegion& region, int choice,
                                           const RegionLineTake& lines) {
    switch (choice) {
    case RegionOurs:
        return region.ours;
    case RegionBase:
        return region.base;
    case RegionTheirs:
        return region.theirs;
    case RegionBoth: {
        std::vector<std::string> both = region.ours;
        both.insert(both.end(), region.theirs.begin(), region.theirs.end());
        return both;
    }
    case RegionBothReverse: {
        std::vector<std::string> both = region.theirs;
        both.insert(both.end(), region.ours.begin(), region.ours.end());
        return both;
    }
    case RegionLines: {
        std::vector<std::string> taken;
        const auto append = [&taken](const std::vector<std::string>& source,
                                     const std::vector<char>& mask) {
            const std::size_t count = std::min(source.size(), mask.size());
            for (std::size_t i = 0; i < count; ++i) {
                if (mask[i]) {
                    taken.push_back(source[i]);
                }
            }
        };
        if (lines.oursFirst) {
            append(region.ours, lines.ours);
            append(region.theirs, lines.theirs);
        } else {
            append(region.theirs, lines.theirs);
            append(region.ours, lines.ours);
        }
        return taken;
    }
    default:
        return {};
    }
}

bool regionResolved(const ConflictRegion& region, int choice, const RegionLineTake& lines) {
    if (!region.balanced) {
        return false;
    }
    switch (choice) {
    case RegionOurs:
    case RegionBase:
    case RegionTheirs:
    case RegionBoth:
    case RegionBothReverse:
        return true;
    case RegionLines:
        return lines.anyTaken();
    default:
        return false;
    }
}

std::vector<LineSpan> oursLineSpans(const ConflictDocument& document) {
    std::vector<LineSpan> spans;
    std::size_t line = 1;
    for (const ConflictPiece& piece : document.pieces) {
        if (!piece.isRegion) {
            line += piece.lines.size();
            continue;
        }
        const ConflictRegion& region = document.regions.at(piece.region);
        spans.push_back(LineSpan{line, region.ours.size()});
        line += region.ours.size();
    }
    return spans;
}

} // namespace gk
