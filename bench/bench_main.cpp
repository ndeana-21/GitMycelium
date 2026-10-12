/// Not a test: no assertion and no timing in ctest, because a benchmark that
/// asserts is a flaky test. scripts/make-big-repo.sh builds a synthetic
/// repository to point it at.

#include "core/types.h"
#include "graph/graph_layout.h"
#include "repo/repo_session.h"

#include <QString>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gk;

namespace {

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void report(const char* what, double ms) {
    std::printf("%-28s %10.2f ms\n", what, ms);
}

void report(const char* what, std::size_t value) {
    std::printf("%-28s %10zu\n", what, value);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <repo> [pages] [page-size]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const int pages = argc > 2 ? std::atoi(argv[2]) : 10;
    const quint32 pageSize = argc > 3 ? static_cast<quint32>(std::atoi(argv[3])) : 200;

    // The same walk the window's model uses, or the graph benchmark would measure
    // a single line.
    const HistoryFilter filter{HistoryScope::AllRefs, {}};

    RepoSession session;
    QString error;
    bool hasMore = false;

    const Clock::time_point openStart = Clock::now();
    if (!session.open(QString::fromStdString(path), &error)) {
        std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), qPrintable(error));
        return 1;
    }
    const std::vector<CommitInfo> first = session.commitsBlocking(0, pageSize, &hasMore, filter);
    report("open + first page", elapsedMs(openStart));

    std::vector<CommitInfo> loaded = first;
    const Clock::time_point pageStart = Clock::now();
    for (int p = 1; p <= pages && hasMore; ++p) {
        const std::vector<CommitInfo> page =
            session.commitsBlocking(static_cast<quint64>(p) * pageSize, pageSize, &hasMore, filter);
        loaded.insert(loaded.end(), page.begin(), page.end());
    }
    report("next pages (total)", elapsedMs(pageStart));

    Clock::time_point start = Clock::now();
    const std::vector<RefInfo> refs = session.refs();
    report("refs", elapsedMs(start));

    start = Clock::now();
    const auto commitsByRef = session.refsByCommit(filter);
    report("refs by commit", elapsedMs(start));

    start = Clock::now();
    const std::vector<FileChange> status = session.status();
    report("status", elapsedMs(start));

    std::size_t fileBytes = 0;
    if (!loaded.empty()) {
        const QString head = QString::fromStdString(loaded.front().id);
        start = Clock::now();
        const std::vector<ChangedFile> changed = session.changedFiles(head);
        report("changed files (HEAD)", elapsedMs(start));
        if (!changed.empty()) {
            const QString file = QString::fromStdString(changed.front().path);
            std::string bytes;
            start = Clock::now();
            session.fileAtRevision(head, file, &bytes, &error);
            report("file content (HEAD)", elapsedMs(start));
            fileBytes = bytes.size();
        }
    }

    start = Clock::now();
    GraphLayout layout;
    layout.setCommits(loaded);
    report("graph layout (one shot)", elapsedMs(start));

    // The window feeds pages in as they arrive, so the append path is the one
    // that has to stay flat; a one-shot layout hides how it grows with depth.
    start = Clock::now();
    GraphLayout paged;
    double worstAppend = 0.0;
    for (std::size_t offset = 0; offset < loaded.size(); offset += pageSize) {
        const std::size_t count = std::min<std::size_t>(pageSize, loaded.size() - offset);
        const std::vector<CommitInfo> slice(loaded.begin() + static_cast<std::ptrdiff_t>(offset),
                                            loaded.begin() +
                                                static_cast<std::ptrdiff_t>(offset + count));
        const Clock::time_point appendStart = Clock::now();
        paged.append(slice);
        worstAppend = std::max(worstAppend, elapsedMs(appendStart));
    }
    report("append all pages (total)", elapsedMs(start));
    report("append worst page", worstAppend);

    std::printf("\n");
    report("paged lanes", static_cast<std::size_t>(paged.laneCount()));
    report("loaded commits", loaded.size());
    report("refs", refs.size());
    report("commits by ref", commitsByRef.size());
    report("status entries", status.size());
    report("file bytes (HEAD)", fileBytes);
    std::printf("%-28s %10d\n", "graph lanes", layout.laneCount());
    return 0;
}
