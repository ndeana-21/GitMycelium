#include "model/commit_model.h"

#include "repo/repo_session.h"
#include "theme/theme.h"

#include <QBrush>
#include <QDate>
#include <QDateTime>
#include <QFont>
#include <QLocale>
#include <QMimeData>
#include <QSize>
#include <QStringList>

#include <algorithm>
#include <string_view>

namespace gk {

const std::vector<RefInfo> CommitModel::kNoRefs{};

namespace {

/// The id of the synthetic working-tree row. Not a hex oid, so it can never
/// collide with a real commit, and recognisable if it ever leaks into a message.
constexpr const char* kWipCommitId = "//wip";

/// The id prefix of a synthetic date-separator row. Like `//wip` it is not a hex
/// oid, so it can never collide with a commit, and `isTimelineRow` recognises it.
constexpr const char* kTimelineIdPrefix = "//timeline:";

/// Where a commit sits in the timeline, and what the separator above it says.
/// `valid` is false for `TimelinePeriod::None` and for a commit with no timestamp.
struct PeriodStamp {
    bool valid = false;
    qint64 key = 0;      ///< commits sharing it belong under one separator
    QString label;
};

/// The period a commit falls in, in the reader's local time. Labels are English
/// because the rest of the chrome is; "Today" and "Yesterday" are the two a reader
/// actually looks for, and the rest are the period's own name.
PeriodStamp periodStamp(qint64 unix, TimelinePeriod period) {
    PeriodStamp out;
    if (period == TimelinePeriod::None || unix <= 0) {
        return out;
    }
    const QDateTime when = QDateTime::fromSecsSinceEpoch(unix);
    const QDate date = when.date();
    const QDate today = QDate::currentDate();
    const QLocale locale = QLocale::c();
    out.valid = true;

    switch (period) {
    case TimelinePeriod::Hour: {
        out.key = unix / 3600;
        const QString clock = when.toString(QStringLiteral("HH:00"));
        if (date == today) {
            out.label = QStringLiteral("Today %1").arg(clock);
        } else if (date == today.addDays(-1)) {
            out.label = QStringLiteral("Yesterday %1").arg(clock);
        } else {
            out.label = locale.toString(date, QStringLiteral("d MMM yyyy")) +
                        QLatin1Char(' ') + clock;
        }
        break;
    }
    case TimelinePeriod::Day:
        out.key = date.toJulianDay();
        if (date == today) {
            out.label = QStringLiteral("Today");
        } else if (date == today.addDays(-1)) {
            out.label = QStringLiteral("Yesterday");
        } else {
            out.label = locale.toString(date, QStringLiteral("d MMM yyyy"));
        }
        break;
    case TimelinePeriod::Week: {
        int year = 0;
        const int week = date.weekNumber(&year);
        out.key = static_cast<qint64>(year) * 53 + week;
        const QDate start = date.addDays(1 - date.dayOfWeek());
        if (year == today.year() && week == today.weekNumber(nullptr)) {
            out.label = QStringLiteral("This week");
        } else if (year == today.addDays(-7).year() &&
                   week == today.addDays(-7).weekNumber(nullptr)) {
            out.label = QStringLiteral("Last week");
        } else {
            out.label = QStringLiteral("Week of %1")
                            .arg(locale.toString(start, QStringLiteral("d MMM")));
        }
        break;
    }
    case TimelinePeriod::Month:
        out.key = static_cast<qint64>(date.year()) * 12 + date.month();
        out.label = locale.toString(date, QStringLiteral("MMMM yyyy"));
        break;
    case TimelinePeriod::Year:
        out.key = date.year();
        out.label = QString::number(date.year());
        break;
    case TimelinePeriod::None:
        out.valid = false;
        break;
    }
    return out;
}

/// A separator row carrying its own label. It has no parents, so the layout draws
/// the lanes straight through it; `data()` answers TimelineRole for it and refuses
/// to treat it as a commit.
CommitInfo timelineRow(TimelinePeriod period, const PeriodStamp& stamp) {
    CommitInfo row;
    row.id = QStringLiteral("%1%2:%3")
                 .arg(QString::fromLatin1(kTimelineIdPrefix))
                 .arg(static_cast<int>(period))
                 .arg(stamp.key)
                 .toStdString();
    row.summary = stamp.label.toStdString();
    return row;
}

/// "// WIP · 2 unstaged, 1 staged". The count is what makes the row worth a
/// glance: the message is not what the reader is looking at, the work is. A node
/// that belongs to another worktree names it, or two dirty checkouts read as two
/// identical rows.
QString wipSummary(int staged, int unstaged, const QString& where = QString()) {
    QStringList parts;
    if (unstaged > 0) {
        parts << QStringLiteral("%1 unstaged").arg(unstaged);
    }
    if (staged > 0) {
        parts << QStringLiteral("%1 staged").arg(staged);
    }
    const QString prefix =
        where.isEmpty() ? QStringLiteral("// WIP") : QStringLiteral("// WIP · %1").arg(where);
    if (parts.isEmpty()) {
        // The row exists because an operation is open, not because anything is
        // modified — a rebase stopped for `edit` on a clean tree.
        return prefix;
    }
    return QStringLiteral("%1 · %2").arg(prefix, parts.join(QStringLiteral(", ")));
}

/// The node type for one working-tree row. The operation state is what the reader
/// must act on, so it outranks "there are edits".
GraphNodeType wipNodeType(const WorktreeWip& wip) {
    if (wip.conflicted > 0) {
        return GraphNodeType::MergeConflict;
    }
    if (wip.rebaseOpen) {
        return GraphNodeType::UnsupportedRebase;
    }
    return GraphNodeType::WorkDir;
}

/// QVector is QList in Qt 6 and no longer offers toStdVector(), so the graph's
/// std::vector is built by hand.
std::vector<CommitInfo> asVector(const QVector<CommitInfo>& rows) {
    return std::vector<CommitInfo>(rows.begin(), rows.end());
}

QString initialsOf(const std::string& name) {
    QString out;
    bool atWordStart = true;
    for (const QChar ch : QString::fromStdString(name)) {
        if (ch.isSpace() || ch == QLatin1Char('.') || ch == QLatin1Char('-')) {
            atWordStart = true;
            continue;
        }
        if (atWordStart) {
            out.append(ch.toUpper());
            if (out.size() == 2) {
                break;
            }
            atWordStart = false;
        }
    }
    if (out.isEmpty() && !name.empty()) {
        out = QString::fromStdString(name).left(2).toUpper();
    }
    return out;
}

} // namespace

CommitModel::CommitModel(RepoSession* session, QObject* parent)
    : QAbstractTableModel(parent), m_session(session) {
    if (m_session) {
        connect(m_session, &RepoSession::commitsLoaded, this, &CommitModel::onPage);
        connect(m_session, &RepoSession::refMetaLoaded, this, &CommitModel::onRefMeta);
    }
}

int CommitModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}

int CommitModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

const CommitInfo* CommitModel::commitAt(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return nullptr;
    }
    return &m_rows.at(row);
}

const QString& CommitModel::wipId() {
    static const QString id = QString::fromLatin1(kWipCommitId);
    return id;
}

int CommitModel::realRowCount() const {
    return m_rows.size() - m_wipCount - m_timelineCount;
}

bool CommitModel::isTimelineRow(int row) const {
    return row >= 0 && row < m_rows.size() &&
           m_rows.at(row).id.rfind(kTimelineIdPrefix, 0) == 0;
}

const CommitInfo* CommitModel::lastRealCommit() const {
    for (int row = m_rows.size() - 1; row >= m_wipCount; --row) {
        if (!isTimelineRow(row)) {
            return &m_rows.at(row);
        }
    }
    return nullptr;
}

Oid CommitModel::headTipFromRefs() const {
    if (m_headBranch.isEmpty()) {
        return {};
    }
    const std::string head = m_headBranch.toStdString();
    for (const auto& [commitId, bucket] : m_refsByCommit) {
        for (const RefInfo& ref : bucket) {
            if (ref.shortName == head) {
                return commitId;
            }
        }
    }
    return {};
}

namespace {

/// Id of a WIP node. The current checkout keeps the original `//wip`, so the row,
/// the tests and the headless selectors keep meaning what they always did; a linked
/// worktree is named by its directory, so two nodes cannot collide.
QString wipIdFor(const WorktreeWip& wip) {
    if (wip.current || wip.path.empty()) {
        return CommitModel::wipId();
    }
    return QStringLiteral("//wip:") + QString::fromStdString(wip.path);
}

std::string baseNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

void CommitModel::setWorktreeWip(const std::vector<WorktreeWip>& list) {
    std::vector<CommitInfo> next;
    next.reserve(list.size());
    int currentRow = -1;
    for (const WorktreeWip& source : list) {
        CommitInfo wip;
        wip.id = wipIdFor(source).toStdString();
        wip.summary =
            wipSummary(source.staged, source.unstaged,
                       source.current ? QString() : QString::fromStdString(source.name))
                .toStdString();
        wip.author.name = "You";
        wip.committer.whenUnix = QDateTime::currentSecsSinceEpoch();
        // The parent is the worktree's own HEAD, so the layout hangs the node off
        // the branch that worktree is on. Read from `git worktree list`, not from the
        // loaded rows: the commit may not be paged in yet, and a node pointing at
        // nothing would be stranded.
        if (!source.head.empty()) {
            wip.parents.push_back(source.head);
        }
        if (source.current) {
            currentRow = static_cast<int>(next.size());
        }
        next.push_back(std::move(wip));
    }

    const bool same = [&] {
        if (static_cast<int>(next.size()) != m_wipCount) {
            return false;
        }
        for (int i = 0; i < m_wipCount; ++i) {
            if (m_rows.at(i).id != next[static_cast<std::size_t>(i)].id) {
                return false;
            }
        }
        return true;
    }();

    if (same) {
        // Same nodes, so only the text or the branch they hang from can have moved.
        bool parentMoved = false;
        for (int i = 0; i < m_wipCount; ++i) {
            const CommitInfo& fresh = next[static_cast<std::size_t>(i)];
            if (m_rows.at(i).parents != fresh.parents) {
                parentMoved = true;
            }
            m_rows[i] = fresh;
        }
        m_currentWipRow = currentRow;
        m_wipInfo = list;
        if (parentMoved) {
            // The branch the node hangs from moved, so its colour may have.
            rebuildTipColors();
            m_graph.setNodeTypeHints(nodeTypeHints());
            m_graph.setCommits(asVector(m_rows));
            m_pathDirty = true;
            emit graphChanged();
        }
        applyGraphNodeTypes();
        if (m_wipCount > 0) {
            emit dataChanged(index(0, 0), index(m_wipCount - 1, ColumnCount - 1));
        }
        return;
    }

    // The set changed — one appeared, one left, or the order moved. Replace the
    // whole block at the front; the real commits shift by the difference, which is
    // exactly what the paging arithmetic accounts for.
    if (m_wipCount > 0) {
        beginRemoveRows(QModelIndex(), 0, m_wipCount - 1);
        for (int i = 0; i < m_wipCount; ++i) {
            m_rows.remove(0);
        }
        m_wipCount = 0;
        m_currentWipRow = -1;
        m_wipInfo.clear();
        endRemoveRows();
    }
    if (!next.empty()) {
        beginInsertRows(QModelIndex(), 0, static_cast<int>(next.size()) - 1);
        for (int i = static_cast<int>(next.size()) - 1; i >= 0; --i) {
            m_rows.insert(0, next[static_cast<std::size_t>(i)]);
        }
        m_wipCount = static_cast<int>(next.size());
        m_currentWipRow = currentRow;
        m_wipInfo = list;
        endInsertRows();
    }
    // The rows changed, so the working-tree nodes' colours do too: each inherits its
    // branch's, which is what keeps it on that branch's line.
    rebuildTipColors();
    m_graph.setNodeTypeHints(nodeTypeHints());
    m_graph.setCommits(asVector(m_rows));
    m_pathDirty = true;
    emit graphChanged();
    applyGraphNodeTypes();
}

void CommitModel::setWorkingTreeChanges(const std::vector<FileChange>& changes) {
    if (changes.empty()) {
        setWorktreeWip({});
        return;
    }
    WorktreeWip wip;
    if (m_session) {
        const RepoInfo info = m_session->info();
        wip.path = info.workDir.empty() ? info.gitDir : info.workDir;
        wip.name = baseNameOf(wip.path);
    }
    wip.head = headTipFromRefs();
    for (const FileChange& change : changes) {
        (change.staged ? wip.staged : wip.unstaged) += 1;
    }
    wip.current = true;
    setWorktreeWip({wip});
}

const GraphNode* CommitModel::graphNodeAt(int row) const {
    if (row < 0 || row >= static_cast<int>(m_graph.size())) {
        return nullptr;
    }
    return &m_graph.node(static_cast<std::size_t>(row));
}

void CommitModel::setPathAnchor(const QString& commitId) {
    if (commitId == m_pathAnchor) {
        return;
    }
    m_pathAnchor = commitId;
    m_pathDirty = true;
    emit highlightChanged();
}

void CommitModel::setPathAnchorRow(int row) {
    setPathAnchor(row >= 0 && row < m_rows.size() && !isTimelineRow(row)
                      ? QString::fromStdString(m_rows.at(row).id)
                      : QString());
}

const BranchPath& CommitModel::branchPath() const {
    if (!m_pathDirty) {
        return m_path;
    }
    m_pathDirty = false;
    m_path = BranchPath{};
    const int rows = static_cast<int>(m_rows.size());
    if (m_pathAnchor.isEmpty() || static_cast<int>(m_graph.size()) != rows) {
        return m_path;
    }
    const std::string anchor = m_pathAnchor.toStdString();
    std::unordered_map<std::string_view, int> rowOf;
    rowOf.reserve(static_cast<std::size_t>(rows));
    int anchorRow = -1;
    for (int row = 0; row < rows; ++row) {
        const std::string& id = m_rows.at(row).id;
        rowOf.emplace(id, row);
        if (id == anchor) {
            anchorRow = row;
        }
    }
    m_path = BranchPath::trace(
        anchorRow, rows, [this](int row) { return &m_graph.node(static_cast<std::size_t>(row)); },
        [this, &rowOf](int row) {
            const std::vector<Oid>& parents = m_rows.at(row).parents;
            if (parents.empty()) {
                return -1;
            }
            const auto found = rowOf.find(parents.front());
            return found == rowOf.end() ? BranchPath::kNotLoaded : found->second;
        });
    return m_path;
}

int CommitModel::rowForCommit(const QString& commitId) const {
    const std::string id = commitId.toStdString();
    const int loaded = static_cast<int>(m_rows.size());
    for (int row = 0; row < loaded; ++row) {
        if (m_rows.at(row).id == id) {
            return row;
        }
    }
    return -1;
}

Oid CommitModel::headTipId() const {
    if (m_headBranch.isEmpty()) {
        return {};
    }
    for (int row = 0; row < m_rows.size(); ++row) {
        for (const RefInfo& ref : refsAt(row)) {
            if (QString::fromStdString(ref.shortName) == m_headBranch) {
                return m_rows.at(row).id;
            }
        }
    }
    return {};
}

std::vector<LineageRow> CommitModel::lineageRows() const {
    std::vector<LineageRow> rows;
    rows.reserve(m_rows.size());
    for (const CommitInfo& commit : m_rows) {
        LineageRow row;
        row.id = commit.id;
        row.parents = commit.parents;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<Oid> CommitModel::headLineage() const {
    return firstParentLineage(lineageRows(), headTipId());
}

const std::vector<RefInfo>& CommitModel::refsAt(int row) const {
    const CommitInfo* ci = commitAt(row);
    if (!ci) {
        return kNoRefs;
    }
    const auto it = m_refsByCommit.find(ci->id);
    return it == m_refsByCommit.end() ? kNoRefs : it->second;
}

void CommitModel::applyGraphNodeTypes() {
    m_graph.setNodeTypeHints(nodeTypeHints());
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, Graph), index(m_rows.size() - 1, Graph));
    }
}

std::unordered_map<std::string, GraphNodeType> CommitModel::nodeTypeHints() const {
    std::unordered_map<std::string, GraphNodeType> hints;

    // The working-tree rows. `m_rows` front block and `m_wipInfo` are in the same
    // order, so the i-th row is the i-th worktree.
    const int wipRows = std::min(m_wipCount, static_cast<int>(m_wipInfo.size()));
    for (int i = 0; i < wipRows; ++i) {
        hints[m_rows.at(i).id] = wipNodeType(m_wipInfo.at(static_cast<std::size_t>(i)));
    }

    // Date separators: the layout has to know about them during the walk, or it
    // would try to place a lane for a row that is not a commit.
    for (int row = m_wipCount; row < m_rows.size(); ++row) {
        if (isTimelineRow(row)) {
            hints[m_rows.at(row).id] = GraphNodeType::Timeline;
        }
    }

    // A stash tip is a stash node whatever its parents make it look like: the
    // stash commit itself is a merge of HEAD and the index, and would otherwise be
    // drawn as a junction.
    for (const auto& [oid, bucket] : m_refsByCommit) {
        for (const RefInfo& ref : bucket) {
            if (ref.fullName == "refs/stash") {
                hints[oid] = GraphNodeType::Stash;
            }
        }
    }

    return hints;
}

int CommitModel::assignColor(const std::string& shortName) {
    const auto known = m_refColors.find(shortName);
    if (known != m_refColors.end()) {
        return known->second;
    }
    // Least-used slot: keeps simultaneous branches visually distinct for as
    // long as the palette lasts, while staying stable for a given name.
    // NOTE: do not name this `slots` — Qt defines that as a macro.
    const int colorSlots = theme::branchColorCount();
    int best = 0;
    for (int i = 1; i < colorSlots; ++i) {
        if (m_colorUse[static_cast<std::size_t>(i)] < m_colorUse[static_cast<std::size_t>(best)]) {
            best = i;
        }
    }
    m_colorUse[static_cast<std::size_t>(best)] += 1;
    m_refColors[shortName] = best;
    return best;
}

int CommitModel::refColorIndex(const QString& shortName) const {
    const auto it = m_refColors.find(shortName.toStdString());
    return it == m_refColors.end() ? 0 : it->second;
}

int CommitModel::refTrackingFlags(const QString& shortName) const {
    // The caller carries a name, not a ref, and means a branch: a tag of the same
    // name must not inherit the branch's arrows.
    const int local = m_refTracking.value(QStringLiteral("refs/heads/") + shortName, 0);
    return local != 0 ? local
                      : m_refTracking.value(QStringLiteral("refs/remotes/") + shortName, 0);
}

CommitModel::RefPillData CommitModel::refPillData(int row) const {
    RefPillData data;
    for (const RefInfo& ref : refsAt(row)) {
        const QString name = QString::fromStdString(ref.shortName);
        data.labels << name;
        data.colors << refColorIndex(name);
        data.kinds << static_cast<int>(ref.kind);
        // Keyed by the full ref: `refs/heads/v1.0` and `refs/tags/v1.0` are two
        // references, and only the branch has an upstream to compare against.
        data.trackings << m_refTracking.value(QString::fromStdString(ref.fullName), 0);
    }
    return data;
}

void CommitModel::refreshRefs() {
    m_refColors.clear();
    m_colorUse.assign(static_cast<std::size_t>(theme::branchColorCount()), 0);
    m_refsByCommit.clear();

    if (!m_session || !m_session->isOpen()) {
        m_graph.setTipColors({});
        m_pathDirty = true;
        applyGraphNodeTypes();
        return;
    }

    m_headBranch = QString::fromStdString(m_session->info().headBranch);
    m_refsByCommit = m_session->refsByCommit(m_historyFilter);
    // Ahead/behind is not something the ref walk answers, and `for-each-ref` over
    // every branch must not run here: this is the GUI thread. The labels show the
    // relation a worker delivers, and the badges appear when it arrives.
    m_session->requestRefMeta();

    // The current branch takes the first colour, then local branches, then
    // remotes and tags, which keeps "your" branch the first colour in the list.
    std::vector<std::string> order;
    if (!m_headBranch.isEmpty()) {
        order.push_back(m_headBranch.toStdString());
    }
    for (const RefKind kind : {RefKind::LocalBranch, RefKind::RemoteBranch, RefKind::Tag}) {
        for (const auto& [oid, bucket] : m_refsByCommit) {
            for (const RefInfo& ref : bucket) {
                if (ref.kind == kind) {
                    order.push_back(ref.shortName);
                }
            }
        }
    }
    std::sort(order.begin(), order.end());
    std::stable_sort(order.begin(), order.end(), [this](const std::string& a, const std::string& b) {
        return (a == m_headBranch.toStdString()) > (b == m_headBranch.toStdString());
    });
    for (const std::string& name : order) {
        assignColor(name);
    }

    rebuildTipColors();
    rebuildTipPriority();
    applyGraphNodeTypes();

    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, Refs), index(m_rows.size() - 1, Refs),
                         {RefsLabelsRole, RefsColorsRole, RefsKindsRole, RefsNamesRole});
    }
}

void CommitModel::rebuildTipColors() {
    m_tipColors.clear();
    // A tip's colour is the colour of the most significant label on it: a local
    // branch outranks a remote-tracking branch, which outranks a tag.
    for (const auto& [commitId, bucket] : m_refsByCommit) {
        if (bucket.empty()) {
            continue;
        }
        m_tipColors[commitId] = refColorIndex(QString::fromStdString(bucket.front().shortName));
    }
    // A working-tree row is not a branch of its own: it sits at the tip of the branch
    // the worktree is on. Its parent is exactly that branch's HEAD, so it takes the
    // parent tip's colour — otherwise the layout finds nothing awaiting it, treats it
    // as a fresh tip, and the uncommitted changes are drawn as a separate branch in a
    // colour no label has.
    for (int i = 0; i < m_wipCount; ++i) {
        const CommitInfo& wip = m_rows.at(i);
        if (wip.parents.empty()) {
            continue;
        }
        const auto parent = m_tipColors.find(wip.parents.front());
        if (parent != m_tipColors.end()) {
            const int parentColor = parent->second;
            m_tipColors[wip.id] = parentColor;
        }
    }
    m_graph.setTipColors(m_tipColors);
    m_pathDirty = true;
}

void CommitModel::rebuildTipPriority() {
    // 0 = the branch you are on, 1 = a branch pinned to the left, 2 = the branch
    // you last clicked, 3 = a labelled branch, and everything unlisted keeps the
    // walk order, which the layout preserves.
    std::unordered_map<std::string, int> priority;
    for (const auto& [commitId, bucket] : m_refsByCommit) {
        if (bucket.empty()) {
            continue;
        }
        int best = 3;
        for (const RefInfo& ref : bucket) {
            const QString name = QString::fromStdString(ref.shortName);
            if (!m_headBranch.isEmpty() && name == m_headBranch) {
                best = 0;
                break;
            }
            if (m_pinnedBranches.contains(name)) {
                best = std::min(best, 1);
            }
            if (!m_focusedBranch.isEmpty() && name == m_focusedBranch) {
                best = std::min(best, 2);
            }
        }
        priority[commitId] = best;
    }
    m_graph.setTipPriority(std::move(priority));
    m_pathDirty = true;
    emit graphChanged();
}

void CommitModel::setRefIcons(bool show) {
    if (m_refIcons == show) {
        return;
    }
    m_refIcons = show;
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, Refs), index(m_rows.size() - 1, Refs));
    }
}

void CommitModel::setCommitDateFormat(CommitDateFormat format) {
    if (m_dateFormat == format) {
        return;
    }
    m_dateFormat = format;
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, Date), index(m_rows.size() - 1, Date));
    }
}

void CommitModel::setTimelinePeriod(TimelinePeriod period) {
    if (m_timelinePeriod == period) {
        return;
    }
    m_timelinePeriod = period;
    // The separators sit *between* rows, so their positions cannot be patched in
    // place: the history restarts from the first page with the new grouping.
    reset();
}

void CommitModel::setHistoryFilter(const HistoryFilter& filter) {    if (m_historyFilter.scope == filter.scope &&
        m_historyFilter.visibleRefs == filter.visibleRefs) {
        return;
    }
    m_historyFilter = filter;
    reset();
}

void CommitModel::setPinnedBranches(const QStringList& names) {
    if (m_pinnedBranches == names) {
        return;
    }
    m_pinnedBranches = names;
    rebuildTipPriority();
}

void CommitModel::setFocusedBranch(const QString& shortName) {
    if (m_focusedBranch == shortName) {
        return;
    }
    m_focusedBranch = shortName;
    rebuildTipPriority();
}

void CommitModel::setHighlightedBranch(const QString& shortName) {
    if (m_highlightBranch == shortName) {
        return;
    }
    m_highlightBranch = shortName;
    rebuildHighlight();
}

void CommitModel::rebuildHighlight() {
    m_highlightSet.clear();
    m_highlightActive = false;

    if (!m_highlightBranch.isEmpty()) {
        // The tip is the commit that wears this branch's label. Read from the refs,
        // not from a stored id: a fetch or a rebase moves the tip under us.
        Oid tip;
        for (int row = 0; row < m_rows.size() && tip.empty(); ++row) {
            for (const RefInfo& ref : refsAt(row)) {
                if (QString::fromStdString(ref.shortName) == m_highlightBranch) {
                    tip = m_rows.at(row).id;
                    break;
                }
            }
        }
        if (!tip.empty()) {
            // Bounded: past the cap there is no useful highlight, and the caller is
            // told so rather than shown an arbitrary slice of the branch.
            constexpr std::size_t kHighlightCap = 20000;
            const std::vector<Oid> ids = ancestorSet(lineageRows(), tip, kHighlightCap);
            if (!ids.empty()) {
                m_highlightSet.insert(ids.begin(), ids.end());
                m_highlightActive = true;
            }
        }
    }

    emit highlightChanged();
}

bool CommitModel::isDimmedRow(int row) const {
    if (!m_highlightActive) {
        return false;
    }
    const CommitInfo* ci = commitAt(row);
    if (!ci) {
        return false;
    }
    return m_highlightSet.find(ci->id) == m_highlightSet.end();
}

Qt::ItemFlags CommitModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags base = QAbstractTableModel::flags(index);
    if (index.isValid()) {
        if (isTimelineRow(index.row())) {
            // A separator is a heading, not a row the reader can act on: it is not
            // selectable, not draggable and not a drop target, so the keyboard skips
            // it and a drop cannot land on it.
            return base & ~(Qt::ItemIsSelectable | Qt::ItemIsDropEnabled |
                            Qt::ItemIsDragEnabled);
        }
        base |= Qt::ItemIsDropEnabled;
        if (index.column() == Refs && !refsAt(index.row()).empty()) {
            base |= Qt::ItemIsDragEnabled;
        }
    } else {
        // The viewport is not a drop target: a drop there has no commit.
        base &= ~Qt::ItemIsDropEnabled;
    }
    return base;
}

QStringList CommitModel::mimeTypes() const {
    return {QString::fromLatin1(kRefMimeType)};
}

bool CommitModel::canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                  const QModelIndex& parent) const {
    Q_UNUSED(action);
    if (!data || !data->hasFormat(QString::fromLatin1(kRefMimeType))) {
        return false;
    }
    if (RefModel::decode(data).shortName.isEmpty()) {
        return false;
    }
    const QModelIndex target = parent.isValid() ? parent : index(row, column, parent);
    if (!target.isValid() || target.row() >= m_rows.size()) {
        return false;
    }
    // A branch does not belong on a working-tree row: there is no commit there
    // to move, create or merge it into.
    if (isWipRow(target.row())) {
        return false;
    }
    // Nor on a date separator, which is not a commit either.
    return !isTimelineRow(target.row());
}

bool CommitModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                               const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent)) {
        return false;
    }
    const QModelIndex target = parent.isValid() ? parent : index(row, column, parent);
    const CommitInfo& commit = m_rows.at(target.row());
    const RefModel::DraggedRef source = RefModel::decode(data);

    // Dropping a label onto another label means merge/rebase; dropping it onto
    // bare commit row means "put this branch here". The view resolves a drop
    // onto a label and calls this with that label as the target row, so we
    // report the label case when the *target commit* carries the same label.
    emit refDroppedOnCommit(source, QString::fromStdString(commit.id));
    return true;
}

Qt::DropActions CommitModel::supportedDropActions() const {
    return Qt::MoveAction | Qt::CopyAction;
}

QVariant CommitModel::data(const QModelIndex& index, int role) const {
    const CommitInfo* ci = commitAt(index.row());
    if (!ci) {
        return {};
    }

    if (isTimelineRow(index.row())) {
        // A separator is a heading: it shows its label in the message column and
        // nothing else, and it is never a commit anything may act on.
        if (role == TimelineRole) {
            return true;
        }
        if (role == Qt::SizeHintRole) {
            return QSize(0, theme::rowHeight());
        }
        if (role == Qt::DisplayRole) {
            return index.column() == Message ? QVariant(QString::fromStdString(ci->summary))
                                             : QVariant();
        }
        if (role == Qt::ForegroundRole) {
            return QBrush(theme::textDim());
        }
        if (role == Qt::ToolTipRole) {
            return QString::fromStdString(ci->summary);
        }
        return {};
    }

    const std::vector<RefInfo>& refs = refsAt(index.row());

    if (role == Qt::SizeHintRole) {
        return QSize(0, theme::rowHeight());
    }

    if (role == WipRole) {
        // True on any working-tree row, whatever column asks.
        return isWipRow(index.row());
    }

    if (role == WipWorktreeRole) {
        return isWipRow(index.row())
                   ? QVariant(QString::fromStdString(m_wipInfo[static_cast<std::size_t>(index.row())].path))
                   : QVariant();
    }

    if (role == WipNameRole) {
        return isWipRow(index.row())
                   ? QVariant(QString::fromStdString(m_wipInfo[static_cast<std::size_t>(index.row())].name))
                   : QVariant();
    }

    if (role == RefsHeadRole) {
        // The delegate needs to know which of the labels is where you are, and it
        // cannot tell from the label text: a branch whose name matches a tag's is
        // still two different things.
        return index.column() == Refs ? QVariant(m_headBranch) : QVariant();
    }

    if (role == RefsLabelsRole || role == RefsColorsRole || role == RefsKindsRole ||
        role == RefsNamesRole) {
        if (index.column() != Refs) {
            return {};
        }
        QVariantList out;
        out.reserve(static_cast<int>(refs.size()));
        for (const RefInfo& ref : refs) {
            const QString name = QString::fromStdString(ref.shortName);
            if (role == RefsLabelsRole) {
                out.push_back(name);
            } else if (role == RefsColorsRole) {
                out.push_back(refColorIndex(name));
            } else if (role == RefsKindsRole) {
                out.push_back(static_cast<int>(ref.kind));
            } else {
                out.push_back(name);
            }
        }
        return out;
    }

    if (role == CommitIdRole) {
        return QString::fromStdString(ci->id);
    }

    if (role == Qt::ForegroundRole) {
        // Dimming is a row fact, so it is answered here and the default delegate
        // picks it up for the text columns. The graph and label columns are painted
        // by their own delegates, which ask isDimmedRow() directly.
        if (isDimmedRow(index.row())) {
            return QBrush(theme::textDim());
        }
        return {};
    }

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Refs:
            // The delegate paints labels; this is the accessible fallback.
            if (refs.empty()) {
                return QString();
            }
            return QString::fromStdString(refs.front().shortName);
        case Graph:
            return QString(); // painted by GraphDelegate
        case Message:
            return QString::fromStdString(ci->summary);
        case Author:
            return QString::fromStdString(ci->author.name);
        case Date: {
            return formatCommitDate(ci->committer.whenUnix, m_dateFormat);
        }
        case Sha:
            return QString::fromStdString(ci->id).left(10);
        default:
            return {};
        }
    }

    if (role == Qt::UserRole && index.column() == Graph) {
        return initialsOf(ci->author.name);
    }
    if (role == AuthorIdentityRole) {
        return QString::fromStdString(ci->author.email.empty() ? ci->author.name
                                                               : ci->author.email);
    }

    if (role == Qt::ToolTipRole) {
        if (index.column() == Refs && !refs.empty()) {
            QStringList lines;
            for (const RefInfo& ref : refs) {
                lines << QStringLiteral("%1  (%2)\n%3")
                             .arg(QString::fromStdString(ref.shortName),
                                  QString::fromUtf8(toString(ref.kind)),
                                  QString::fromStdString(ref.target));
            }
            return lines.join(QStringLiteral("\n\n"));
        }
        return QString::fromStdString(ci->id) + QLatin1Char('\n') +
               QString::fromStdString(ci->summary);
    }

    if (role == Qt::FontRole && index.column() == Sha) {
        QFont f(QStringLiteral("monospace"));
        return f;
    }

    return {};
}

QVariant CommitModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return {};
    }
    switch (section) {
    case Refs: return QStringLiteral("Branch / Tag");
    case Graph: return QStringLiteral("Graph");
    case Message: return QStringLiteral("Commit Message");
    case Author: return QStringLiteral("Author");
    case Date: return QStringLiteral("Date");
    case Sha: return QStringLiteral("SHA");
    default: return {};
    }
}

bool CommitModel::canFetchMore(const QModelIndex& /*parent*/) const {
    return false;
}

bool CommitModel::canLoadMore() const {
    return m_session && m_session->isOpen() && m_hasMore && !m_loading;
}

void CommitModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || !m_session || m_loading || !m_hasMore) {
        return;
    }
    m_loading = true;
    emit loadingChanged();
    m_session->requestCommits(m_generation, static_cast<quint64>(realRowCount()),
                              RepoSession::kPageSize,
                              m_historyFilter);
}

void CommitModel::onPage(CommitPage page) {
    // A page asked for before a reset belongs to a history that is no longer on screen;
    // `skip` alone cannot tell, since a reset starts again from 0. It also must not
    // clear the loading flag of the request that is still in flight for this one.
    if (page.generation != m_generation) {
        return;
    }
    m_loading = false;
    emit loadingChanged();
    if (!page.error.isEmpty()) {
        onFailed(page.error);
        return;
    }
    if (page.skip != static_cast<quint64>(realRowCount())) {
        return;
    }
    if (page.commits.isEmpty()) {
        m_hasMore = false;
        return;
    }

    // Interleave the date separators before the rows are inserted, so a single
    // beginInsertRows/endInsertRows covers commits and separators together.
    std::vector<CommitInfo> block;
    block.reserve(static_cast<std::size_t>(page.commits.size()) * 2);
    const CommitInfo* previous = lastRealCommit();
    qint64 previousKey = 0;
    bool havePrevious = false;
    if (previous) {
        const PeriodStamp stamp = periodStamp(previous->committer.whenUnix, m_timelinePeriod);
        havePrevious = stamp.valid;
        previousKey = stamp.key;
    }
    int separators = 0;
    for (const CommitInfo& commit : page.commits) {
        const PeriodStamp stamp = periodStamp(commit.committer.whenUnix, m_timelinePeriod);
        if (stamp.valid && (!havePrevious || stamp.key != previousKey)) {
            block.push_back(timelineRow(m_timelinePeriod, stamp));
            ++separators;
        }
        if (stamp.valid) {
            previousKey = stamp.key;
            havePrevious = true;
        }
        block.push_back(commit);
    }

    const int first = m_rows.size();
    const int last = first + static_cast<int>(block.size()) - 1;
    beginInsertRows(QModelIndex(), first, last);
    for (const CommitInfo& row : block) {
        m_rows.push_back(row);
    }
    m_timelineCount += separators;
    // The separators are in `m_rows` now, so the hints name them before the walk
    // sees them; the walk needs that to skip a lane for a row that is not a commit.
    m_graph.setNodeTypeHints(nodeTypeHints());
    m_graph.append(block);
    m_pathDirty = true;
    endInsertRows();
    m_hasMore = page.hasMore;
    emit graphChanged();
    // The loaded range grew, so a branch's ancestry may now reach further. The
    // highlight is recomputed here rather than lazily, because the delegate asks
    // "is this row dimmed" on every paint and must not walk the graph there.
    if (!m_highlightBranch.isEmpty()) {
        rebuildHighlight();
    }
}

void CommitModel::onFailed(QString message) {
    m_loading = false;
    emit loadingChanged();
    m_hasMore = false;
    emit headerDataChanged(Qt::Vertical, 0, 0);
    Q_UNUSED(message);
}

void CommitModel::onRefMeta(gk::RefMetaSnapshot snapshot) {
    // A snapshot read for a repository that has since been closed is not this
    // model's to apply.
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    m_refTracking.clear();
    for (const RefMeta& meta : snapshot.meta) {
        const int flags = (meta.ahead ? 1 : 0) | (meta.behind ? 2 : 0);
        if (flags == 0) {
            continue;
        }
        m_refTracking.insert(QString::fromStdString(meta.fullName), flags);
    }
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, Refs), index(m_rows.size() - 1, Refs),
                         {RefsLabelsRole, RefsColorsRole, RefsKindsRole, RefsNamesRole});
    }
}

void CommitModel::reset() {
    ++m_generation;
    beginResetModel();
    m_rows.clear();
    m_graph.reset();
    m_pathDirty = true;
    m_wipCount = 0;
    m_currentWipRow = -1;
    m_wipInfo.clear();
    m_timelineCount = 0;
    m_hasMore = true;
    m_loading = false;
    emit loadingChanged();
    // The rows are gone, so the highlight set is meaningless until they reload.
    // The branch stays remembered and `onPage` rebuilds it.
    m_highlightSet.clear();
    m_highlightActive = false;
    refreshRefs();
    endResetModel();
    // Seed the first page; the view pages on from there as the reader scrolls.
    if (canLoadMore()) {
        fetchMore(QModelIndex());
    }
}

} // namespace gk
