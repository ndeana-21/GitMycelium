/// The history walk is git's own `--topo-order`, implemented incrementally over the
/// commit-graph. git is the oracle: every shape here is compared row for row with
/// `git rev-list --topo-order`, with no graph, a graph, a split chain, a graph that
/// is out of date, and one that is broken.

#include "core/types.h"
#include "git/cli.h"
#include "git/commit_graph.h"
#include "git/topo_walk.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

#include <cstring>

using namespace gk;

namespace {

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).exitCode == 0;
}

QString gitOut(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

QStringList walked(RepoSession& session, const HistoryFilter& filter, quint32 pageSize) {
    QStringList ids;
    bool more = true;
    while (more) {
        const auto page = session.commitsBlocking(static_cast<quint64>(ids.size()), pageSize,
                                                  &more, filter);
        if (page.empty()) {
            break;
        }
        for (const CommitInfo& commit : page) {
            ids << QString::fromStdString(commit.id);
        }
    }
    return ids;
}

} // namespace

class TopoWalkTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void withoutAGraphTheOrderIsGits();
    void withAGraphTheOrderIsGits();
    void theGraphAloneCarriesTheWalk();
    void aSplitChainThatIsOutOfDateIsStillGits();
    void headOnlyIsGitsHead();
    void visibleRefsAreWalkedInGitsOrder();
    void aBrokenGraphIsIgnored();
    void theRawHeaderGivesParentsAndDate();
    void aLongHistoryIsIndexedOnceInTheBackground();
    void indexingNeedsToBeAskedFor();

private:
    bool commit(const QString& name, int date);
    bool importLinearHistory(const QString& path, int commits);
    void loadFirstPage(RepoSession& session);
    void buildHistory();
    void compareWithGit(const QString& what);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void TopoWalkTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(git(m_path, {"config", "gc.auto", "0"}));
    QVERIFY(git(m_path, {"config", "fetch.writeCommitGraph", "false"}));
    buildHistory();
}

void TopoWalkTest::cleanup() {
    m_dir.reset();
}

bool TopoWalkTest::commit(const QString& name, int date) {
    QFile file(QDir(m_path).filePath(name + QStringLiteral(".txt")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(name.toUtf8());
    file.close();
    const QByteArray stamp = QByteArray::number(1700000000 + date) + " +0000";
    qputenv("GIT_AUTHOR_DATE", stamp);
    qputenv("GIT_COMMITTER_DATE", stamp);
    return git(m_path, {"add", "-A"}) && git(m_path, {"commit", "-q", "-m", name});
}

void TopoWalkTest::buildHistory() {
    // Dates repeat on purpose (ties are where an order is decided by the rules
    // rather than by the clock) and one runs backwards (a child older than its
    // parent must still come first).
    QVERIFY(commit(QStringLiteral("c1"), 10));
    QVERIFY(commit(QStringLiteral("c2"), 20));
    QVERIFY(git(m_path, {"branch", "side", "HEAD~1"}));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commit(QStringLiteral("f1"), 30));
    QVERIFY(git(m_path, {"tag", "-a", "-m", "annotated", "v-annotated"}));
    QVERIFY(commit(QStringLiteral("f2"), 30));
    QVERIFY(git(m_path, {"checkout", "-q", "side"}));
    QVERIFY(commit(QStringLiteral("s1"), 25));
    QVERIFY(commit(QStringLiteral("s2"), 5));
    QVERIFY(git(m_path, {"tag", "v-light"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commit(QStringLiteral("c3"), 30));
    QVERIFY(git(m_path, {"merge", "-q", "--no-ff", "-m", "merge feature", "feature"}));
    for (const QString& octo : {QStringLiteral("o1"), QStringLiteral("o2"), QStringLiteral("o3")}) {
        QVERIFY(git(m_path, {"checkout", "-q", "-b", octo, "main"}));
        QVERIFY(commit(octo, 40));
    }
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(git(m_path, {"merge", "-q", "--no-ff", "-m", "octopus", "o1", "o2", "o3"}));
    QVERIFY(commit(QStringLiteral("c4"), 50));

    QFile dirty(QDir(m_path).filePath(QStringLiteral("c4.txt")));
    QVERIFY(dirty.open(QIODevice::WriteOnly | QIODevice::Truncate));
    dirty.write("stashed");
    dirty.close();
    QVERIFY(git(m_path, {"stash", "push", "-q", "-m", "wip"}));
}

void TopoWalkTest::compareWithGit(const QString& what) {
    // A stash is drawn with its first parent only: the commits holding the index and
    // the untracked files are how git stores it, not history.
    QStringList expected =
        gitOut(m_path, {"rev-list", "--topo-order", "--all"}).split(QLatin1Char('\n'));
    expected.removeAll(gitOut(m_path, {"rev-parse", "refs/stash^2"}));
    for (const quint32 pageSize : {quint32{1}, quint32{3}, quint32{1000}}) {
        RepoSession session;
        QVERIFY(session.open(m_path));
        const QStringList ids =
            walked(session, HistoryFilter{HistoryScope::AllRefs, {}}, pageSize);
        if (ids != expected) {
            qWarning().noquote() << what << "page size" << pageSize << "\nours:\n"
                                 << ids.join(QLatin1Char('\n')) << "\ngit:\n"
                                 << expected.join(QLatin1Char('\n'));
        }
        QCOMPARE(ids, expected);
    }
}

void TopoWalkTest::withoutAGraphTheOrderIsGits() {
    QVERIFY(!CommitGraph::open((m_path + QStringLiteral("/.git/objects")).toStdString()));
    compareWithGit(QStringLiteral("no graph"));
}

void TopoWalkTest::withAGraphTheOrderIsGits() {
    QVERIFY(git(m_path, {"commit-graph", "write", "--reachable"}));
    const auto graph = CommitGraph::open((m_path + QStringLiteral("/.git/objects")).toStdString());
    QVERIFY(graph);
    QCOMPARE(graph->size(),
             gitOut(m_path, {"rev-list", "--all", "--count"}).toUInt());
    compareWithGit(QStringLiteral("graph"));
}

void TopoWalkTest::theGraphAloneCarriesTheWalk() {
    // Nothing may be read outside the graph: a walk that quietly fell back to the
    // object database would pass every other test here and still be slow.
    QVERIFY(git(m_path, {"commit-graph", "write", "--reachable"}));
    const auto graph = CommitGraph::open((m_path + QStringLiteral("/.git/objects")).toStdString());
    QVERIFY(graph);

    std::vector<TopoWalk::RawOid> tips;
    QStringList starts = gitOut(m_path, {"for-each-ref", "--format=%(refname)"})
                             .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    starts << QStringLiteral("HEAD");
    for (const QString& start : starts) {
        const QByteArray hex =
            gitOut(m_path, {"rev-parse", start + QStringLiteral("^{commit}")}).toLatin1();
        const QByteArray bytes = QByteArray::fromHex(hex);
        QCOMPARE(bytes.size(), 20);
        TopoWalk::RawOid raw;
        std::memcpy(raw.data(), bytes.constData(), raw.size());
        tips.push_back(raw);
    }

    TopoWalk walk(graph.get(), [](const TopoWalk::RawOid&, WalkCommit&) { return false; });
    walk.start(tips);
    QStringList ids;
    TopoWalk::RawOid id;
    while (walk.next(id)) {
        ids << QString::fromLatin1(
            QByteArray(reinterpret_cast<const char*>(id.data()), static_cast<int>(id.size()))
                .toHex());
    }
    QCOMPARE(walk.readOutsideGraph(), std::size_t{0});
    QCOMPARE(ids, gitOut(m_path, {"rev-list", "--topo-order", "--all"}).split(QLatin1Char('\n')));
}

void TopoWalkTest::aSplitChainThatIsOutOfDateIsStillGits() {
    QVERIFY(git(m_path, {"commit-graph", "write", "--reachable", "--split"}));
    QVERIFY(git(m_path, {"checkout", "-q", "side"}));
    QVERIFY(commit(QStringLiteral("s3"), 60));
    QVERIFY(git(m_path, {"commit-graph", "write", "--reachable", "--split=no-merge"}));
    QVERIFY(QFile::exists(m_path +
                          QStringLiteral("/.git/objects/info/commit-graphs/commit-graph-chain")));
    // Written after the last layer: the walk must read these outside the graph.
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commit(QStringLiteral("c5"), 15));
    QVERIFY(commit(QStringLiteral("c6"), 70));
    compareWithGit(QStringLiteral("split chain"));
}

void TopoWalkTest::headOnlyIsGitsHead() {
    QVERIFY(git(m_path, {"commit-graph", "write", "--reachable"}));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));
    RepoSession session;
    QVERIFY(session.open(m_path));
    const QStringList expected =
        gitOut(m_path, {"rev-list", "--topo-order", "HEAD"}).split(QLatin1Char('\n'));
    QCOMPARE(walked(session, HistoryFilter{HistoryScope::HeadOnly, {}}, 2), expected);
}

void TopoWalkTest::visibleRefsAreWalkedInGitsOrder() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    const HistoryFilter filter{HistoryScope::AllRefs, {"refs/heads/side", "refs/heads/feature"}};
    // git sorts its starting points by date, keeping the given order for a tie;
    // ours are given by name, then HEAD.
    const QStringList expected =
        gitOut(m_path, {"rev-list", "--topo-order", "refs/heads/feature", "refs/heads/side", "HEAD"})
            .split(QLatin1Char('\n'));
    QCOMPARE(walked(session, filter, 4), expected);
}

void TopoWalkTest::aBrokenGraphIsIgnored() {
    QVERIFY(git(m_path, {"commit-graph", "write", "--reachable"}));
    const QString file = m_path + QStringLiteral("/.git/objects/info/commit-graph");
    QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);
    QFile graph(file);
    QVERIFY(graph.open(QIODevice::ReadWrite));
    QVERIFY(graph.resize(graph.size() / 2));
    graph.close();
    QVERIFY(!CommitGraph::open((m_path + QStringLiteral("/.git/objects")).toStdString()));
    compareWithGit(QStringLiteral("broken graph"));
}

void TopoWalkTest::theRawHeaderGivesParentsAndDate() {
    const QString merge = gitOut(m_path, {"rev-parse", "main~1"});
    const QString raw = gitOut(m_path, {"cat-file", "commit", merge});
    const std::string bytes = raw.toStdString();
    WalkCommit parsed;
    QVERIFY(parseCommitHeader(bytes.data(), bytes.size(), parsed));

    const QStringList parents = gitOut(m_path, {"log", "-1", "--format=%P", merge})
                                    .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QCOMPARE(parents.size(), 4);
    QCOMPARE(parsed.parents.size(), static_cast<std::size_t>(parents.size()));
    for (int i = 0; i < parents.size(); ++i) {
        QString hex;
        for (const unsigned char byte : parsed.parents[static_cast<std::size_t>(i)]) {
            hex += QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0'));
        }
        QCOMPARE(hex, parents[i]);
    }
    QCOMPARE(parsed.time, gitOut(m_path, {"log", "-1", "--format=%ct", merge}).toLongLong());
}

bool TopoWalkTest::importLinearHistory(const QString& path, int commits) {
    QByteArray stream;
    for (int i = 1; i <= commits; ++i) {
        const QByteArray message = "c" + QByteArray::number(i);
        stream += "commit refs/heads/main\n";
        stream += "committer T <t@t> " + QByteArray::number(1600000000 + i) + " +0000\n";
        stream += "data " + QByteArray::number(message.size()) + "\n" + message + "\n\n";
    }
    QProcess import;
    import.setWorkingDirectory(path);
    import.start(QStringLiteral("git"), {QStringLiteral("fast-import"), QStringLiteral("--quiet")});
    if (!import.waitForStarted()) {
        return false;
    }
    import.write(stream);
    import.closeWriteChannel();
    return import.waitForFinished(60000) && import.exitCode() == 0;
}

void TopoWalkTest::loadFirstPage(RepoSession& session) {
    QSignalSpy loaded(&session, &RepoSession::commitsLoaded);
    session.requestCommits(1, 0, 200, HistoryFilter{HistoryScope::AllRefs, {}});
    QVERIFY(loaded.wait(30000));
}

void TopoWalkTest::aLongHistoryIsIndexedOnceInTheBackground() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(git(dir.path(), {"init", "-q", "-b", "main"}));
    QVERIFY(importLinearHistory(dir.path(), 10001));
    const QString chain =
        dir.path() + QStringLiteral("/.git/objects/info/commit-graphs/commit-graph-chain");

    {
        RepoSession session;
        QVERIFY(session.open(dir.path()));
        session.setAutoCommitGraph(true);
        loadFirstPage(session);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(chain), 30000);
    }
    const auto graph = CommitGraph::open((dir.path() + QStringLiteral("/.git/objects")).toStdString());
    QVERIFY(graph);
    QCOMPARE(graph->size(), 10001U);
}

void TopoWalkTest::indexingNeedsToBeAskedFor() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(git(dir.path(), {"init", "-q", "-b", "main"}));
    QVERIFY(importLinearHistory(dir.path(), 10001));
    const QString info = dir.path() + QStringLiteral("/.git/objects/info");

    {
        RepoSession session;
        QVERIFY(session.open(dir.path()));
        loadFirstPage(session);
    }
    QVERIFY(!QFile::exists(info + QStringLiteral("/commit-graphs")));

    // Asked for, but the reader turned the graph off: git would not read one.
    QVERIFY(git(dir.path(), {"config", "core.commitGraph", "false"}));
    {
        RepoSession session;
        QVERIFY(session.open(dir.path()));
        session.setAutoCommitGraph(true);
        loadFirstPage(session);
    }
    QVERIFY(!QFile::exists(info + QStringLiteral("/commit-graphs")));
}

QTEST_GUILESS_MAIN(TopoWalkTest)
#include "test_topo_walk.moc"
