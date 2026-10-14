#pragma once

#include "core/selection_plan.h"
#include "core/types.h"
#include "graph/branch_path.h"
#include "graph/graph_layout.h"
#include "model/date_format.h"
#include "model/ref_model.h"
#include "repo/repo_session.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QStringList>
#include <QVariantList>
#include <QVector>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gk {

class RepoSession;

/// Lazily paged commit table. Each fetchMore() asks the session for the next
/// page in the background; rows are appended when it arrives. This keeps the
/// GUI responsive and memory bounded on million-commit histories.
///
/// The history covers *every* reference, not just HEAD: a graph that only shows
/// the current branch is not a graph.
class CommitModel : public QAbstractTableModel {
    Q_OBJECT

public:
    /// Labels come first, then the graph, then the message — the reason a
    /// branch label lines up with its line.
    enum Column { Refs = 0, Graph, Message, Author, Date, Sha, ColumnCount };

    /// Structured label data for the Refs column. A delegate cannot recover
    /// several labels, their colours and their names from one display string.
    enum Role {
        RefsLabelsRole = Qt::UserRole + 1,
        RefsColorsRole,
        RefsKindsRole,
        RefsNamesRole,                     ///< short names (drag payload)
        CommitIdRole,
        RefsHeadRole,
        WipRole,
        WipWorktreeRole,
        WipNameRole,
        TimelineRole,
        AuthorIdentityRole,                ///< author email (or name): the avatar's colour key
    };

    explicit CommitModel(RepoSession* session, QObject* parent = nullptr);

signals:
    /// A page request started or finished (`isLoading`).
    void loadingChanged();

    /// Emitted when the graph lane layout changed and the graph column must be
    /// repainted (the commit rows themselves did not change).
    void graphChanged();

    /// The hover highlight's branch changed, so every visible row may need
    /// repainting: the dimming lives in the rows, not in the layout.
    void highlightChanged();

    /// A reference was dropped onto a commit row ("drag a branch onto a
    /// commit"). The window decides what that means.
    void refDroppedOnCommit(gk::RefModel::DraggedRef source, QString commitId);

    /// A reference was dropped onto *another reference* inside the graph's
    /// label column. Distinct from the case above: the target is a branch, not
    /// a commit, and the meaning is merge/rebase rather than move/create.
    void refDroppedOnRefInGraph(gk::RefModel::DraggedRef source, QString targetShortName);

public:
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    /// Always false: the graph view decides when to page (`CommitGraphView::fetchIfNearEnd`).
    /// QTreeView asks for another page on every layout, and a page that lands causes a
    /// layout, so answering true here loaded the whole history while the window sat idle.
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    /// Another page can be asked for now: there is more history and none is in flight.
    bool canLoadMore() const;

    /// Commits loaded so far, without the working-tree rows and date separators.
    int realRowCount() const;
    /// True while a page is in flight. `canLoadMore` is false then, which is not
    /// the same as "no more history" — a caller paging on its own behalf has to
    /// tell the two apart.
    bool isLoading() const { return m_loading; }
    bool hasMore() const { return m_hasMore; }

    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                         const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;
    Qt::DropActions supportedDropActions() const override;

    /// Safe to call when no repository is open.
    void reset();

    /// Re-reads the reference decorations without touching the loaded rows.
    void refreshRefs();

    /// How the history groups under date separators. Changing it restarts the
    /// whole model: the separators sit between rows, so their positions cannot be
    /// patched in place. `None` removes them.
    void setTimelinePeriod(TimelinePeriod period);
    TimelinePeriod timelinePeriod() const { return m_timelinePeriod; }
    bool isTimelineRow(int row) const;

    /// The branch the user clicked — focused, but not checked out. Focusing a
    /// branch pulls its line to the left so it stays visible even when the graph
    /// column is too narrow for every lane.
    void setFocusedBranch(const QString& shortName);
    QString focusedBranch() const { return m_focusedBranch; }

    /// Whether the label column draws its kind glyphs. Read by the delegate and by
    /// the view's hit-testing, so it lives here and not on either of them: a pill
    /// painted with an icon but hit-tested without one is off by the icon's width.
    void setRefIcons(bool show);
    bool refIcons() const { return m_refIcons; }

    /// How the Date column reads. The window owns the choice; the model only paints
    /// it, so a format change is one setter and one repaint.
    void setCommitDateFormat(CommitDateFormat format);
    CommitDateFormat commitDateFormat() const { return m_dateFormat; }

    /// The refs the history walk starts from, and which carries labels. Changing
    /// it restarts the history: hiding a branch changes which commits exist.
    void setHistoryFilter(const HistoryFilter& filter);

    /// Branches pinned to the left of the graph. Lane order is HEAD, then these,
    /// then the focused branch, then everything else.
    void setPinnedBranches(const QStringList& names);

    /// The branch whose commits the graph keeps lit, dimming the rest. Empty
    /// clears it. Transient: it follows the pointer, and nothing is persisted.
    void setHighlightedBranch(const QString& shortName);
    void clearHighlight() { setHighlightedBranch(QString()); }
    bool highlightActive() const { return m_highlightActive; }
    bool isDimmedRow(int row) const;
    QString highlightedBranch() const { return m_highlightBranch; }

    const gk::CommitInfo* commitAt(int row) const;

    /// The working tree's own row, "// WIP".
    ///
    /// It is synthetic: there is no commit object, and its only parent is one
    /// worktree's HEAD, so the graph hangs it off the branch being worked on. Every
    /// dirty worktree gets one; they live *in* `m_rows` at the front rather than
    /// beside it, which is what keeps every row index in the rest of the code
    /// meaning the same thing it always did.
    bool hasWip() const { return m_wipCount > 0; }
    /// Row of the current checkout's node, or -1 when that checkout is clean. Callers
    /// that mean "the working tree I am in" want this, not just any WIP row.
    int wipRow() const { return m_currentWipRow; }
    bool isWipRow(int row) const { return row >= 0 && row < m_wipCount; }
    static const QString& wipId();

    /// Replaces the working-tree rows from the given set: one per dirty worktree,
    /// the current checkout first. An empty list removes them all.
    void setWorktreeWip(const std::vector<WorktreeWip>& list);

    /// Creates, updates or removes the working-tree row from the current changes.
    /// Read whenever status is refreshed; an empty list removes the row. The
    /// single-worktree shape, kept for the callers and tests that only ever mean
    /// "my working tree".
    void setWorkingTreeChanges(const std::vector<FileChange>& changes);

    /// Row of a loaded commit, or -1 when it is not in the loaded range.
    int rowForCommit(const QString& commitId) const;

    /// Labels attached to a commit, in stacking order (branches, then remotes,
    /// then tags). Empty for a commit no reference points at.
    const std::vector<RefInfo>& refsAt(int row) const;
    /// Whether a remote branch whose tip sits on the trunk's line keeps its label.
    void setShowMergedRemotes(bool show);
    bool showMergedRemotes() const { return m_showMergedRemotes; }

    /// Palette index of a reference's colour — stable for the whole session so
    /// a branch keeps its colour across refreshes.
    int refColorIndex(const QString& shortName) const;

    /// The parallel lists `RefPillLayout::arrange` needs for one row, built in one
    /// place so the delegate that paints a pill and the view that hit-tests one are
    /// never handed different lists. A difference between the two is a width
    /// difference: a pill that looks one way and is grabbed another.
    struct RefPillData {
        QStringList labels;
        QVariantList colors;
        QVariantList kinds;
        QVariantList trackings; ///< one int per label: 1 ahead, 2 behind, 3 both
    };
    RefPillData refPillData(int row) const;

    /// How a reference relates to its upstream: 1 ahead, 2 behind, 3 diverged, 0
    /// when it has none or is level. One lookup for the callers that carry a name
    /// but not a bucket — the hover badge and the reveal overlay.
    int refTrackingFlags(const QString& shortName) const;

    QString headBranchName() const { return m_headBranch; }
    /// Moves on with every `reset`: a page or a decision taken for an older history.
    quint64 generation() const { return m_generation; }

    /// The id the checked-out branch points at, empty when that commit is not loaded.
    Oid headTipId() const;

    /// The loaded commits in the shape the lineage walk wants: id and parents only.
    std::vector<LineageRow> lineageRows() const;

    /// The checked-out branch's own line over the loaded commits, newest first. Empty when
    /// the branch tip is not among them — then nothing loaded can be called a commit of
    /// this branch, and the caller must say so rather than guess.
    std::vector<Oid> headLineage() const;

    const GraphNode* graphNodeAt(int row) const;
    int laneCount() const { return m_graph.laneCount(); }

    /// The commit whose branch the graph traces: its line up to the branch tip and
    /// the first-parent chain below, through every branch it was cut from.
    void setPathAnchor(const QString& commitId);
    void setPathAnchorRow(int row);
    QString pathAnchor() const { return m_pathAnchor; }
    /// Traced on first use after the graph or the anchor changed.
    const BranchPath& branchPath() const;

private slots:
    void onPage(gk::CommitPage page);
    void onFailed(QString message);
    void onRefMeta(gk::RefMetaSnapshot snapshot);

private:
    void rebuildTipColors();
    void rebuildTipPriority();
    bool isTrunk(const RefInfo& ref) const;
    void ensureShownRefs() const;
    void rebuildHighlight();
    /// Relabels the graph nodes the commit list cannot classify on its own: the
    /// working-tree rows, date separators and the tip of `refs/stash`.
    void applyGraphNodeTypes();
    /// Built on demand so the layout can consult it *during* a walk (a separator
    /// takes no lane) rather than after.
    std::unordered_map<std::string, GraphNodeType> nodeTypeHints() const;
    int assignColor(const std::string& shortName);

    /// HEAD's commit id read from the refs themselves, so the synthetic WIP row can
    /// point at the branch before the commit it sits on has been paged in.
    Oid headTipFromRefs() const;
    /// The newest loaded commit, skipping separators and working-tree rows; null
    /// when nothing but those are loaded.
    const CommitInfo* lastRealCommit() const;

    RepoSession* m_session = nullptr;
    QVector<CommitInfo> m_rows;
    GraphLayout m_graph;
    int m_wipCount = 0;
    int m_currentWipRow = -1;
    /// What each of the front `m_wipCount` rows stands for, in the same order.
    std::vector<WorktreeWip> m_wipInfo;
    bool m_hasMore = true;
    bool m_loading = false;
    /// Bumped by every reset; a page carries the value it was requested with.
    quint64 m_generation = 0;

    std::unordered_map<std::string, std::vector<RefInfo>> m_refsByCommit;
    /// Ref short name -> palette index, kept for the lifetime of the model so
    /// colours do not shuffle on every refresh.
    std::unordered_map<std::string, int> m_refColors;
    std::vector<int> m_colorUse;
    /// Full ref name (`refs/heads/main`) -> tracking flags (1 ahead, 2 behind),
    /// from the asynchronous `for-each-ref` snapshot. Full names, not short ones,
    /// so a tag beside a branch of the same name cannot borrow its arrows. Only
    /// refs that differ from their upstream are present.
    QHash<QString, int> m_refTracking;
    std::unordered_map<std::string, int> m_tipColors;
    QString m_headBranch;
    QString m_trunkBranch;
    QString m_focusedBranch;
    bool m_refIcons = true;
    CommitDateFormat m_dateFormat = CommitDateFormat::Iso;
    /// How the loaded history groups under date separators, and how many of
    /// `m_rows` are such separators rather than commits.
    TimelinePeriod m_timelinePeriod = TimelinePeriod::None;
    int m_timelineCount = 0;
    /// Refs the walk starts from and labels. Defaults to every ref, the graph's
    /// normal view; an empty `visibleRefs` means all.
    HistoryFilter m_historyFilter{HistoryScope::AllRefs, {}};
    QStringList m_pinnedBranches;
    /// The hovered branch and the commits it lights: its tip and ancestry in the
    /// loaded range. `m_highlightActive` is false when nothing is highlighted, or
    /// when the branch's history was too large to compute a useful set.
    QString m_highlightBranch;
    std::unordered_set<Oid> m_highlightSet;
    bool m_highlightActive = false;
    QString m_pathAnchor;
    mutable BranchPath m_path;
    mutable bool m_pathDirty = true;
    /// The labels drawn: `m_refsByCommit` without the remote branches parked on the
    /// trunk's line (`ensureShownRefs`), rebuilt lazily after any graph change.
    mutable std::unordered_map<std::string, std::vector<RefInfo>> m_shownRefs;
    mutable bool m_shownDirty = true;
    bool m_showMergedRemotes = false;
    static const std::vector<RefInfo> kNoRefs;
};

} // namespace gk
