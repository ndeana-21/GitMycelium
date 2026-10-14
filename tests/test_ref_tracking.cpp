
#include "git/cli.h"
#include "model/commit_model.h"
#include "repo/repo_session.h"
#include "ui/commit_graph_view.h"
#include "ui/ref_pill_layout.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <memory>

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
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

bool writeFile(const QString& path, const QString& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}

} // namespace

class RefTrackingTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theBadgeWidensThePillAndSitsInsideIt();
    void bothDirectionsDrawBothArrows();
    void aLevelBranchHasNoBadge();
    void aMissingTrackingEntryIsNoBadgeNotACrash();
    void paintingAndHitTestingAgreeWithATrackingBadge();
    void theModelFillsTrackingFromTheRefMetaSnapshot();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void RefTrackingTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();

    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(m_path + "/a.txt", QStringLiteral("base\n")));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));

    // The upstream is a local remote-tracking ref, not a real remote: `trackshort`
    // reads it the same way, and the test needs no network. The remote still has to
    // be *declared* — `branch.*.remote` is ignored otherwise, and every branch would
    // look like it has no upstream at all.
    QVERIFY(git(m_path, {"remote", "add", "origin", m_path + "/origin.git"}));
    const QString base = gitOut(m_path, {"rev-parse", "HEAD"});
    const auto trackTo = [&](const QString& branch, const QString& upstreamRef,
                             const QString& tip) {
        QVERIFY(git(m_path, {"update-ref", upstreamRef, tip}));
        QVERIFY(git(m_path, {"config", "branch." + branch + ".remote", "origin"}));
        QVERIFY(git(m_path, {"config", "branch." + branch + ".merge", "refs/heads/" + branch}));
    };

    // `ahead`: one commit its upstream lacks, and a tag of the same name beside it
    // — a tag is not a branch and must not borrow the branch's arrows.
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "ahead"}));
    trackTo(QStringLiteral("ahead"), QStringLiteral("refs/remotes/origin/ahead"), base);
    QVERIFY(writeFile(m_path + "/a.txt", QStringLiteral("base\nahead\n")));
    QVERIFY(git(m_path, {"commit", "-qam", "ahead 1"}));
    QVERIFY(git(m_path, {"tag", "ahead"}));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "behind", "main"}));
    trackTo(QStringLiteral("behind"), QStringLiteral("refs/remotes/origin/behind"), base);
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "tmp", "main"}));
    QVERIFY(writeFile(m_path + "/a.txt", QStringLiteral("base\nremote\n")));
    QVERIFY(git(m_path, {"commit", "-qam", "remote only"}));
    const QString remoteOnly = gitOut(m_path, {"rev-parse", "HEAD"});
    QVERIFY(git(m_path, {"update-ref", "refs/remotes/origin/behind", remoteOnly}));
    QVERIFY(git(m_path, {"checkout", "-q", "behind"}));

    trackTo(QStringLiteral("main"), QStringLiteral("refs/remotes/origin/main"), base);
}

void RefTrackingTest::cleanup() {
    m_dir.reset();
}

void RefTrackingTest::theBadgeWidensThePillAndSitsInsideIt() {
    const QRect cell(0, 0, 240, 30);
    const auto plain =
        RefPillLayout::arrange({QStringLiteral("main")}, {0}, {0}, {0}, cell);
    const auto tracked =
        RefPillLayout::arrange({QStringLiteral("main")}, {0}, {0}, {1}, cell);
    QCOMPARE(plain.size(), static_cast<std::size_t>(1));
    QCOMPARE(tracked.size(), static_cast<std::size_t>(1));

    QVERIFY(plain.front().badgeRect.isNull());
    QVERIFY(plain.front().trackingText.isEmpty());

    const RefPill& pill = tracked.front();
    QVERIFY(pill.badgeRect.isValid());
    QCOMPARE(pill.trackingText, QStringLiteral("\u2191"));
    QVERIFY(pill.rect.width() > plain.front().rect.width());
    QVERIFY(pill.rect.contains(pill.badgeRect));
    QVERIFY2(pill.badgeRect.left() > pill.textRect.right(),
             qPrintable(QStringLiteral("badge overlaps the name: badge %1..%2, text ..%3")
                            .arg(pill.badgeRect.left())
                            .arg(pill.badgeRect.right())
                            .arg(pill.textRect.right())));
    QVERIFY(pill.iconRect.isValid());
    QVERIFY(pill.badgeRect.right() < pill.iconRect.left());

    const RefPill* hit = RefPillLayout::hitTest(tracked, pill.badgeRect.center());
    QVERIFY(hit != nullptr);
    QCOMPARE(hit->shortName, QStringLiteral("main"));
}

void RefTrackingTest::bothDirectionsDrawBothArrows() {
    QCOMPARE(RefPillLayout::trackingText(1), QStringLiteral("\u2191"));
    QCOMPARE(RefPillLayout::trackingText(2), QStringLiteral("\u2193"));
    QCOMPARE(RefPillLayout::trackingText(3), QStringLiteral("\u2191\u2193"));
    QVERIFY(RefPillLayout::trackingText(0).isEmpty());

    const QRect cell(0, 0, 240, 30);
    const auto one = RefPillLayout::arrange({QStringLiteral("main")}, {0}, {0}, {1}, cell);
    const auto both = RefPillLayout::arrange({QStringLiteral("main")}, {0}, {0}, {3}, cell);
    QCOMPARE(both.front().trackingText, QStringLiteral("\u2191\u2193"));
    QVERIFY(both.front().rect.width() > one.front().rect.width());
    QVERIFY(both.front().badgeRect.width() > one.front().badgeRect.width());
}

void RefTrackingTest::aLevelBranchHasNoBadge() {
    const QRect cell(0, 0, 240, 30);
    const auto pills =
        RefPillLayout::arrange({QStringLiteral("main")}, {0}, {0}, {0}, cell);
    QVERIFY(pills.front().badgeRect.isNull());
    QVERIFY(pills.front().trackingText.isEmpty());
}

void RefTrackingTest::aMissingTrackingEntryIsNoBadgeNotACrash() {
    const QRect cell(0, 0, 240, 30);
    // Fewer tracking entries than labels: the missing ones are "no badge", and the
    // arrangement is the same shape as if they had been zeros.
    const auto short0 = RefPillLayout::arrange(
        {QStringLiteral("main"), QStringLiteral("v1.0")}, {0, 2}, {0, 2}, {}, cell);
    const auto zeros = RefPillLayout::arrange(
        {QStringLiteral("main"), QStringLiteral("v1.0")}, {0, 2}, {0, 2}, {0, 0}, cell);
    QCOMPARE(short0.size(), zeros.size());
    for (std::size_t i = 0; i < short0.size(); ++i) {
        QCOMPARE(short0.at(i).rect, zeros.at(i).rect);
        QVERIFY(short0.at(i).badgeRect.isNull());
    }
}

void RefTrackingTest::paintingAndHitTestingAgreeWithATrackingBadge() {
    const QRect cell(0, 0, 240, 30);
    const QStringList labels{QStringLiteral("main"), QStringLiteral("v2.0")};
    const QVariantList colors{0, 2};
    const QVariantList kinds{0, 2};
    const QVariantList trackings{3, 1};

    // The delegate and the view both call arrange with the model's lists; calling it
    // twice is that same contract, and the two must be identical or a pill is painted
    // in one place and grabbed in another.
    const auto painted = RefPillLayout::arrange(labels, colors, kinds, trackings, cell);
    const auto hit = RefPillLayout::arrange(labels, colors, kinds, trackings, cell);
    QCOMPARE(painted.size(), hit.size());
    for (std::size_t i = 0; i < painted.size(); ++i) {
        QCOMPARE(painted.at(i).rect, hit.at(i).rect);
        QCOMPARE(painted.at(i).textRect, hit.at(i).textRect);
        QCOMPARE(painted.at(i).badgeRect, hit.at(i).badgeRect);
        const RefPill* resolved = RefPillLayout::hitTest(hit, painted.at(i).rect.center());
        QVERIFY(resolved != nullptr);
        QCOMPARE(resolved->shortName, painted.at(i).shortName);
    }
}

void RefTrackingTest::theModelFillsTrackingFromTheRefMetaSnapshot() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QTRY_VERIFY(model.rowCount() >= 1);

    QTRY_COMPARE(model.refTrackingFlags(QStringLiteral("ahead")), 1);
    QCOMPARE(model.refTrackingFlags(QStringLiteral("behind")), 2);
    QCOMPARE(model.refTrackingFlags(QStringLiteral("main")), 0);
    QCOMPARE(model.refTrackingFlags(QStringLiteral("tmp")), 0);

    const int row = model.rowForCommit(gitOut(m_path, {"rev-parse", "refs/heads/ahead"}));
    QVERIFY(row >= 0);
    const CommitModel::RefPillData data = model.refPillData(row);
    QCOMPARE(data.labels.size(), data.colors.size());
    QCOMPARE(data.labels.size(), data.kinds.size());
    QCOMPARE(data.labels.size(), data.trackings.size());

    // The row carries the branch *and* a tag of the same name; only the branch has
    // an upstream to compare against.
    const std::vector<RefInfo>& refs = model.refsAt(row);
    QCOMPARE(static_cast<qsizetype>(refs.size()), data.labels.size());
    int branchIndex = -1;
    int tagIndex = -1;
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        if (refs.at(static_cast<std::size_t>(i)).fullName == "refs/heads/ahead") {
            branchIndex = i;
        } else if (refs.at(static_cast<std::size_t>(i)).fullName == "refs/tags/ahead") {
            tagIndex = i;
        }
    }
    QVERIFY(branchIndex >= 0);
    QVERIFY(tagIndex >= 0);
    QCOMPARE(data.trackings.at(branchIndex).toInt(), 1);
    QCOMPARE(data.trackings.at(tagIndex).toInt(), 0);

    // What the view lays out for the row — the pills the hit test resolves — must be
    // the same rectangles the delegate's own lists produce. This is the width
    // agreement: if the view ever drops the tracking list, its badge disappears here
    // and the two no longer match.
    CommitGraphView view;
    view.resize(1400, 800);
    view.setColumnWidth(CommitModel::Refs, 260);
    view.setModel(&model);
    const QModelIndex refsIndex = model.index(row, CommitModel::Refs);
    const QRect cell = view.visualRect(refsIndex).adjusted(2, 0, -2, 0);
    const QString head = model.data(refsIndex, CommitModel::RefsHeadRole).toString();
    const auto direct = RefPillLayout::arrange(data.labels, data.colors, data.kinds,
                                               data.trackings, cell, head, model.focusedBranch(),
                                               model.refIcons());
    const std::vector<RefPill> viaView = view.rowPillsForDiagnostics(row);
    QCOMPARE(viaView.size(), direct.size());
    QVERIFY(!viaView.empty());
    for (std::size_t i = 0; i < viaView.size(); ++i) {
        QCOMPARE(viaView.at(i).rect, direct.at(i).rect);
        QCOMPARE(viaView.at(i).badgeRect, direct.at(i).badgeRect);
    }

    const auto branchPill = std::find_if(viaView.begin(), viaView.end(), [](const RefPill& p) {
        return p.trackingText == QStringLiteral("\u2191");
    });
    QVERIFY(branchPill != viaView.end());
    QVERIFY(branchPill->badgeRect.isValid());
    QCOMPARE(std::count_if(viaView.begin(), viaView.end(),
                           [](const RefPill& p) { return p.trackingText.isEmpty(); }),
             static_cast<std::ptrdiff_t>(viaView.size() - 1));
}

QTEST_MAIN(RefTrackingTest)
#include "test_ref_tracking.moc"
