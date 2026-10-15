#pragma once

#include "core/types.h"
#include "model/path_tree.h"

#include <QColor>
#include <QStandardItemModel>
#include <QString>
#include <QStringList>

#include <vector>

namespace gk {

/// Painted in the UI layer, which this header has no business pulling in.
enum class Glyph;

/// Payload: "fullName|shortName|target|kind" — see RefModel::mimeData().
inline constexpr const char* kRefMimeType = "application/x-gitmycelium-ref";

class RefModel : public QStandardItemModel {
    Q_OBJECT

public:
    /// What a row is. Drag-and-drop and the context menu both need this: with
    /// folders in the tree, "is it a child of a top-level row" no longer means
    /// anything.
    enum ItemType { SectionItem = 0, FolderItem, RefItem, StashItem, WorktreeItem, SubmoduleItem };

    enum Role {
        FullNameRole = Qt::UserRole,
        TargetRole,
        KindRole,
        ShortNameRole,
        ItemTypeRole,
        /// Prefix of the folder a row lives in, with a trailing slash. Empty at
        /// the top. A folder row stores its own prefix here, which is also how a
        /// folder gets pinned: `feature/` is a different pin from `feature`.
        FolderRole,
        PinnedRole,
        /// Stash rows only: the position at read time, and the oid an operation is
        /// addressed by.
        StashIndexRole,
        StashOidRole,
        WorktreePathRole,
        WorktreeCurrentRole,
        WorktreeLockedRole,
        SubmodulePathRole,
        SubmoduleUrlRole,
        SubmoduleStateRole,
        /// The eye in the rail: `EyeNone`, `EyeShown` or `EyeHidden`. A folder or a
        /// section is hidden when every branch under it is.
        EyeRole,
    };
    enum Eye { EyeNone = 0, EyeShown, EyeHidden };

    explicit RefModel(QObject* parent = nullptr);

    /// Replaces the contents. Remembered, because the filter and the pin list
    /// both rebuild the same rows.
    void rebuild(const std::vector<RefInfo>& refs);
    void refresh();

    /// Replaces the stash section. `messages` and `oids` are parallel, newest
    /// first, so position i is `stash@{i}`; the section is hidden when empty.
    /// Stashes are not references — they have no upstream, no pin and no drag —
    /// which is why they are handed in beside the refs rather than as `RefInfo`s.
    void setStashes(const QStringList& messages, const QStringList& oids);

    void setWorktrees(const std::vector<WorktreeEntry>& worktrees);

    void setSubmodules(const std::vector<SubmoduleEntry>& submodules);

    void setHeadBranch(const QString& branch);

    /// Names pinned to the top, in the order they should appear. A folder is
    /// named with a trailing slash. Pinned rows are *moved* out of their section,
    /// not duplicated.
    void setPinned(const QStringList& names);

    /// Branches the graph is filtering out, so the tree can mark them. `solo`, when
    /// non-empty, wins over `hidden` exactly as it does in the history filter, and
    /// HEAD is never marked.
    void setVisibility(const QStringList& hidden, const QStringList& solo);

    /// Shows only references matching `pattern`.
    ///
    /// Without `regex` it is a case-insensitive substring match, i.e. the input
    /// is wrapped in `*…*`. With `regex` the pattern is used as it is typed.
    void setFilter(const QString& pattern, bool regex);
    QString filterError() const { return m_filterError; }
    bool filterActive() const { return !m_filter.isEmpty(); }

    /// Pin key of a row: the branch name, or the folder's prefix with a trailing
    /// slash. Null for a section. One definition, so the menu that pins and the
    /// model that reads pins cannot disagree about what a name means.
    static QString pinKey(const QModelIndex& index);

    static bool isRef(const QModelIndex& index);
    /// Every branch at or under `index` the eye can hide (not the current branch).
    static QStringList eyeBranchNames(const QModelIndex& index);

    static bool isStash(const QModelIndex& index);

    static bool isWorktree(const QModelIndex& index);

    static bool isSubmodule(const QModelIndex& index);

    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                         const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;
    Qt::DropActions supportedDropActions() const override;

    struct DraggedRef {
        QString fullName;
        QString shortName;
        QString target;
        RefKind kind = RefKind::Other;
    };
    static DraggedRef decode(const QMimeData* data);

    /// Short name of a reference row.
    ///
    /// DisplayRole is *not* safe for this: the current branch is decorated
    /// (bold, accent colour, "current" in the tooltip) and decorating the text
    /// would leak into the branch name used by git operations.
    static QString refShortName(const QModelIndex& index);

    static RefKind refKind(const QModelIndex& index);

    /// Drag payload for a reference: `fullName|shortName|target|kind`.
    ///
    /// Owned by the model so the views cannot invent a second, subtly different
    /// encoding — they only ever hand back what they were given.
    static QMimeData* mimeDataForRef(const RefInfo& ref);

    static QMimeData* mimeDataForIndex(const QModelIndex& index);

signals:
    void refDroppedOnRef(gk::RefModel::DraggedRef source, QString targetShortName);

private:
    void appendSection(const QString& title, RefKind kind);
    void appendStashes();
    /// Adds the Worktrees section, one flat row per linked working tree. Hidden
    /// when there is only the main worktree — the section would say "you are here".
    void appendWorktrees();
    void appendSubmodules();
    struct EyeCount {
        int branches = 0;
        int hidden = 0;
    };
    static EyeCount markEyes(QStandardItem* item);
    void appendPinned();
    void appendFolderContents(QStandardItem* parentItem, const PathFolder& folder,
                              const std::vector<const RefInfo*>& items) const;
    QStandardItem* makeSectionItem(const QString& title, Glyph glyph, const QColor& colour) const;
    QStandardItem* makeFolderItem(const PathFolder& folder) const;
    /// `label` is what the row shows: inside a folder it is the part of the name
    /// left after the folder's prefix, and at the top level — or pinned, where
    /// there is no folder to lean on — it is the whole name.
    QStandardItem* makeRefItem(const RefInfo& ref, const QString& label) const;

    bool matches(const QString& name) const;
    bool isMovedToPinned(const RefInfo& ref) const;
    bool isGraphHidden(const RefInfo& ref) const;

    std::vector<RefInfo> m_refs;
    bool m_built = false;
    QString m_builtHeadBranch;
    QStringList m_stashMessages;
    QStringList m_stashOids;
    std::vector<WorktreeEntry> m_worktrees;
    std::vector<SubmoduleEntry> m_submodules;
    QString m_headBranch;
    QStringList m_pinned;
    QStringList m_hidden;
    QStringList m_solo;
    QString m_filter;
    QString m_filterError;
    bool m_regex = false;
};

} // namespace gk

// Needed so QSignalSpy (and queued connections) can carry the drag payload.
Q_DECLARE_METATYPE(gk::RefModel::DraggedRef)
