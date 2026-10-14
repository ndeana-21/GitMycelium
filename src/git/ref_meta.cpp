#include "git/ref_meta.h"

#include "git/cli.h"

#include <cstdlib>
#include <sstream>

namespace gk {
namespace {

std::vector<std::string> splitFields(const std::string& line, char separator) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = line.find(separator, start);
        if (at == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, at - start));
        start = at + 1;
    }
    return fields;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::string stripBrackets(std::string text) {
    if (!text.empty() && text.front() == '<') {
        text.erase(text.begin());
    }
    if (!text.empty() && text.back() == '>') {
        text.pop_back();
    }
    return text;
}

} // namespace

RefMetaOps::RefMetaOps(std::string workDir) : m_workDir(std::move(workDir)) {}

std::vector<RefMeta> RefMetaOps::read() const {
    std::vector<RefMeta> out;
    if (m_workDir.empty()) {
        return out;
    }

    // Tab-separated, because a branch name can contain a space. `%09` is the tab
    // escape for-each-ref understands. `trackshort` is used instead of `track` on
    // purpose: `track` spells "ahead"/"behind" in the machine's language, and this
    // has to parse the same on a Russian box as on an English one.
    const ProcessResult process = runGit(
        {"for-each-ref",
         "--format=%(refname)%09%(refname:short)%09%(authorname)%09%(authoremail)%09"
         "%(committerdate:unix)%09%(upstream:short)%09%(upstream:trackshort)",
         "refs/heads", "refs/remotes"},
        m_workDir);
    if (process.exitCode != 0) {
        return out;
    }

    for (const std::string& line : splitLines(process.out)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> fields = splitFields(line, '\t');
        if (fields.size() < 7) {
            continue;
        }
        RefMeta meta;
        meta.fullName = fields[0];
        meta.shortName = fields[1];
        meta.authorName = fields[2];
        meta.authorEmail = stripBrackets(fields[3]);
        meta.commitUnix = std::strtoll(fields[4].c_str(), nullptr, 10);
        meta.upstream = fields[5];
        const std::string track = fields[6];
        meta.ahead = track.find('>') != std::string::npos;
        meta.behind = track.find('<') != std::string::npos;
        meta.local = meta.fullName.rfind("refs/heads/", 0) == 0;
        out.push_back(std::move(meta));
    }
    return out;
}

} // namespace gk
