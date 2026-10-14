#include "core/unified_diff.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace gk {
namespace {

bool isOctalToken(const std::string& token) {
    if (token.size() < 3 || token.size() > 7) {
        return false;
    }
    for (const char c : token) {
        if (c < '0' || c > '7') {
            return false;
        }
    }
    return true;
}

int octalValue(const std::string& token) {
    return static_cast<int>(std::strtol(token.c_str(), nullptr, 8));
}

/// `a/foo` or `b/foo` -> `foo`; `/dev/null` -> empty. The prefixes are forced by the
/// git invocation (`--src-prefix=a/ --dst-prefix=b/`), so this is exact.
std::string stripPrefix(const std::string& path, const std::string& prefix) {
    if (path == "/dev/null") {
        return {};
    }
    if (path.rfind(prefix, 0) == 0) {
        return path.substr(prefix.size());
    }
    return path;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

/// `@@ -oldStart[,oldLines] +newStart[,newLines] @@ [section]`. Missing counts mean 1.
bool parseHunkHeader(const std::string& line, DiffHunk& hunk) {
    if (line.rfind("@@ -", 0) != 0) {
        return false;
    }
    std::size_t pos = 4;
    const auto readInt = [&line, &pos](int& out) {
        if (pos >= line.size() || !std::isdigit(static_cast<unsigned char>(line[pos]))) {
            return false;
        }
        int value = 0;
        while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos]))) {
            value = value * 10 + (line[pos] - '0');
            ++pos;
        }
        out = value;
        return true;
    };

    int oldLines = 1;
    int newLines = 1;
    if (!readInt(hunk.oldStart)) {
        return false;
    }
    if (pos < line.size() && line[pos] == ',') {
        ++pos;
        if (!readInt(oldLines)) {
            return false;
        }
    }
    if (pos + 1 >= line.size() || line[pos] != ' ' || line[pos + 1] != '+') {
        return false;
    }
    pos += 2;
    if (!readInt(hunk.newStart)) {
        return false;
    }
    if (pos < line.size() && line[pos] == ',') {
        ++pos;
        if (!readInt(newLines)) {
            return false;
        }
    }
    hunk.oldLines = oldLines;
    hunk.newLines = newLines;
    return true;
}

std::string pathAfter(const std::string& line, const char* marker, const std::string& prefix) {
    if (!startsWith(line, marker)) {
        return {};
    }
    return stripPrefix(line.substr(std::string(marker).size()), prefix);
}

} // namespace

std::vector<FileDiff> parseUnifiedDiff(const std::string& text) {
    std::vector<FileDiff> out;
    FileDiff current;
    bool haveCurrent = false;
    bool inHunk = false;
    int nextOld = 0;
    int nextNew = 0;

    // libgit2 names the path on both sides even for a creation or a deletion (only the
    // oid is zero on the absent side), and the rest of the app reads the diff that way.
    const auto pushCurrent = [&out](FileDiff& diff) {
        if (diff.kind == ChangeKind::Added && diff.oldPath.empty()) {
            diff.oldPath = diff.newPath;
        } else if (diff.kind == ChangeKind::Deleted && diff.newPath.empty()) {
            diff.newPath = diff.oldPath;
        }
        out.push_back(std::move(diff));
    };

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line =
            end == std::string::npos ? text.substr(start) : text.substr(start, end - start);
        const bool last = end == std::string::npos;
        start = last ? text.size() + 1 : end + 1;

        if (startsWith(line, "diff --git ")) {
            if (haveCurrent) {
                pushCurrent(current);
            }
            current = FileDiff{};
            haveCurrent = true;
            inHunk = false;
            continue;
        }
        if (!haveCurrent) {
            continue;
        }

        if (startsWith(line, "@@ ")) {
            DiffHunk hunk;
            if (!parseHunkHeader(line, hunk)) {
                continue;
            }
            current.hunks.push_back(hunk);
            nextOld = hunk.oldStart;
            nextNew = hunk.newStart;
            inHunk = true;
            continue;
        }

        if (line == "\\ No newline at end of file") {
            if (!current.hunks.empty() && !current.hunks.back().lines.empty()) {
                current.hunks.back().lines.back().noNewline = true;
            }
            continue;
        }

        // The `---`/`+++` file headers only appear before the first hunk; once a hunk
        // has started a line may legitimately begin with `---` (a removed `-- x`).
        if (!inHunk && startsWith(line, "--- ")) {
            const std::string path = pathAfter(line, "--- ", "a/");
            current.oldPath = path;
            continue;
        }
        if (!inHunk && startsWith(line, "+++ ")) {
            const std::string path = pathAfter(line, "+++ ", "b/");
            current.newPath = path;
            // A `+++ /dev/null` names a deletion; the mode/kind was set above it.
            continue;
        }

        if (startsWith(line, "new file mode ")) {
            current.kind = ChangeKind::Added;
            current.newMode = octalValue(line.substr(14));
            continue;
        }
        if (startsWith(line, "deleted file mode ")) {
            current.kind = ChangeKind::Deleted;
            current.oldMode = octalValue(line.substr(18));
            continue;
        }
        if (startsWith(line, "old mode ")) {
            current.oldMode = octalValue(line.substr(9));
            continue;
        }
        if (startsWith(line, "new mode ")) {
            current.newMode = octalValue(line.substr(9));
            continue;
        }
        if (startsWith(line, "rename from ")) {
            current.kind = ChangeKind::Renamed;
            current.oldPath = line.substr(12);
            continue;
        }
        if (startsWith(line, "rename to ")) {
            current.kind = ChangeKind::Renamed;
            current.newPath = line.substr(10);
            continue;
        }
        if (startsWith(line, "copy from ")) {
            current.kind = ChangeKind::Renamed; // ChangeKind has no separate Copy
            current.oldPath = line.substr(10);
            continue;
        }
        if (startsWith(line, "copy to ")) {
            current.kind = ChangeKind::Renamed;
            current.newPath = line.substr(8);
            continue;
        }
        if (startsWith(line, "Binary files ")) {
            current.binary = true;
            // "Binary files a/x and b/y differ"
            const std::string rest = line.substr(13);
            const std::size_t andPos = rest.find(" and ");
            if (andPos != std::string::npos) {
                const std::string oldSide = rest.substr(0, andPos);
                std::string newSide = rest.substr(andPos + 5);
                const std::size_t differPos = newSide.rfind(" differ");
                if (differPos != std::string::npos) {
                    newSide = newSide.substr(0, differPos);
                }
                current.oldPath = stripPrefix(oldSide, "a/");
                current.newPath = stripPrefix(newSide, "b/");
            }
            continue;
        }
        if (startsWith(line, "index ")) {
            const std::string rest = line.substr(6);
            const std::size_t lastSpace = rest.rfind(' ');
            if (lastSpace != std::string::npos) {
                const std::string mode = rest.substr(lastSpace + 1);
                if (isOctalToken(mode)) {
                    const int value = octalValue(mode);
                    current.oldMode = value;
                    current.newMode = value;
                }
            }
            continue;
        }
        if (startsWith(line, "similarity index ") || startsWith(line, "dissimilarity index ")) {
            continue;
        }

        if (inHunk && !line.empty()) {
            DiffLine diffLine;
            const char marker = line.front();
            if (marker != ' ' && marker != '+' && marker != '-') {
                continue;
            }
            if (marker == ' ') {
                diffLine.kind = DiffLineKind::Context;
                diffLine.oldLine = nextOld++;
                diffLine.newLine = nextNew++;
            } else if (marker == '+') {
                diffLine.kind = DiffLineKind::Added;
                diffLine.oldLine = -1;
                diffLine.newLine = nextNew++;
                ++current.added;
            } else {
                diffLine.kind = DiffLineKind::Removed;
                diffLine.oldLine = nextOld++;
                diffLine.newLine = -1;
                ++current.removed;
            }
            diffLine.text = line.substr(1);
            if (!diffLine.text.empty() && diffLine.text.back() == '\r') {
                diffLine.text.pop_back();
            }
            current.hunks.back().lines.push_back(std::move(diffLine));
        }
    }

    if (haveCurrent) {
        pushCurrent(current);
    }
    return out;
}

std::vector<ChangedFile> parseNameStatus(const std::string& text) {
    std::vector<ChangedFile> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string status = line.substr(0, tab);
        const std::string rest = line.substr(tab + 1);
        const char code = status.empty() ? 'M' : status.front();

        ChangedFile file;
        switch (code) {
        case 'A':
            file.kind = ChangeKind::Added;
            break;
        case 'D':
            file.kind = ChangeKind::Deleted;
            break;
        case 'T':
            file.kind = ChangeKind::TypeChange;
            break;
        case 'U':
            file.kind = ChangeKind::Conflicted;
            break;
        case 'R':
        case 'C':
            file.kind = ChangeKind::Renamed;
            break;
        default:
            file.kind = ChangeKind::Modified;
            break;
        }

        if (code == 'R' || code == 'C') {
            // "R100\told\tnew"; the caller suppresses these with --no-renames, but read
            // them correctly if they appear.
            const std::size_t second = rest.find('\t');
            if (second != std::string::npos) {
                file.oldPath = rest.substr(0, second);
                file.path = rest.substr(second + 1);
            } else {
                file.path = rest;
                file.oldPath = rest;
            }
        } else {
            file.path = rest;
            file.oldPath = rest;
        }
        out.push_back(std::move(file));
    }
    return out;
}

} // namespace gk
