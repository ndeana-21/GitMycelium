#include "model/ref_model.h"

#include "theme/theme.h"
#include "theme/glyphs.h"

#include <QFont>
#include <QMimeData>
#include <QRegularExpression>
#include <QStandardItem>

#include <algorithm>

namespace gk {
namespace {

constexpr int kRowIconSide = 14;

/// Everything up to the last slash, with a trailing slash: the folder a name
/// lives in. Empty for a name with no slash.
QString prefixOf(const QString& name) {
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : name.left(slash + 1);
}

Glyph glyphFor(RefKind kind) {
    // A leaf is a branch whatever it tracks; the panel's section header is what
    // says whether it is local or remote.
    return kind == RefKind::Tag ? Glyph::Tag : Glyph::Branch;
}

bool lessNoCase(const QString& a, const QString& b) {
    return QString::compare(a, b, Qt::CaseInsensitive) < 0;
}

} // namespace

RefModel::RefModel(QObject* parent) : QStandardItemModel(parent) {
    setHorizontalHeaderLabels({QStringLiteral("Reference")});
}

void RefModel::rebuild(const std::vector<RefInfo>& refs) {
    // A refresh hands the same refs back far more often than not (a file saved in the
    // work tree refreshes the window too), and on thousands of branches the tree
    // takes tens of milliseconds to build.
    const bool same =
        m_built && m_builtHeadBranch == m_headBranch && refs.size() == m_refs.size() &&
        std::equal(refs.begin(), refs.end(), m_refs.begin(),
                   [](const RefInfo& a, const RefInfo& b) {
                       return a.kind == b.kind && a.target == b.target && a.fullName == b.fullName;
                   });
    if (same) {
        return;
    }
    m_refs = refs;
    m_built = true;
    m_builtHeadBranch = m_headBranch;
    refresh();
}

void RefModel::setHeadBranch(const QString& branch) {
    m_headBranch = branch;
}

void RefModel::setPinned(const QStringList& names) {
    if (m_pinned == names) {
        return;
    }
    m_pinned = names;
    refresh();
}

void RefModel::setFilter(const QString& pattern, bool regex) {
    if (m_filter == pattern && m_regex == regex) {
        return;
    }
    m_filter = pattern;
    m_regex = regex;
    refresh();
}

void RefModel::setVisibility(const QStringList& hidden, const QStringList& solo) {
    if (m_hidden == hidden && m_solo == solo) {
        return;
    }
    m_hidden = hidden;
    m_solo = solo;
    if (!m_refs.empty()) {
        refresh();
    }
}

void RefModel::setStashes(const QStringList& messages, const QStringList& oids) {
    if (m_stashMessages == messages && m_stashOids == oids) {
        return;
    }
    m_stashMessages = messages;
    m_stashOids = oids;
    refresh();
}

void RefModel::setWorktrees(const std::vector<WorktreeEntry>& worktrees) {
    const bool same = [&] {
        if (m_worktrees.size() != worktrees.size()) {
            return false;
        }
        for (std::size_t i = 0; i < worktrees.size(); ++i) {
            const WorktreeEntry& a = m_worktrees[i];
            const WorktreeEntry& b = worktrees[i];
            if (a.path != b.path || a.branch != b.branch || a.current != b.current ||
                a.locked != b.locked || a.prunable != b.prunable) {
                return false;
            }
        }
        return true;
    }();
    if (same) {
        return;
    }
    m_worktrees = worktrees;
    refresh();
}

void RefModel::setSubmodules(const std::vector<SubmoduleEntry>& submodules) {
    const bool same = [&] {
        if (m_submodules.size() != submodules.size()) {
            return false;
        }
        for (std::size_t i = 0; i < submodules.size(); ++i) {
            const SubmoduleEntry& a = m_submodules[i];
            const SubmoduleEntry& b = submodules[i];
            if (a.path != b.path || a.url != b.url || a.state != b.state ||
                a.initialized != b.initialized || a.checkedOutOid != b.checkedOutOid) {
                return false;
            }
        }
        return true;
    }();
    if (same) {
        return;
    }
    m_submodules = submodules;
    refresh();
}

QString RefModel::pinKey(const QModelIndex& index) {
    if (!index.isValid()) {
        return {};
    }
    if (isRef(index)) {
        return refShortName(index);
    }
    if (index.data(ItemTypeRole).toInt() == FolderItem) {
        // Stored with its trailing slash already, so a folder pin and a branch of
        // the same name are two different entries.
        return index.data(FolderRole).toString();
    }
    return {};
}

bool RefModel::isRef(const QModelIndex& index) {
    return index.isValid() && index.data(ItemTypeRole).toInt() == RefItem;
}

bool RefModel::isStash(const QModelIndex& index) {
    return index.isValid() && index.data(ItemTypeRole).toInt() == StashItem;
}

bool RefModel::isWorktree(const QModelIndex& index) {
    return index.isValid() && index.data(ItemTypeRole).toInt() == WorktreeItem;
}

bool RefModel::isSubmodule(const QModelIndex& index) {
    return index.isValid() && index.data(ItemTypeRole).toInt() == SubmoduleItem;
}

bool RefModel::matches(const QString& name) const {
    if (m_filter.isEmpty()) {
        return true;
    }
    if (m_regex) {
        const QRegularExpression re(m_filter, QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) {
            // An invalid pattern matches nothing — the honest answer, and
            // filterError() carries the reason. Matching anyway would only emit a
            // Qt warning per row.
            return false;
        }
        return re.match(name).hasMatch();
    }
    return name.contains(m_filter, Qt::CaseInsensitive);
}

bool RefModel::isMovedToPinned(const RefInfo& ref) const {
    if (m_pinned.isEmpty()) {
        return false;
    }
    const QString name = QString::fromStdString(ref.shortName);
    if (m_pinned.contains(name)) {
        return true;
    }
    for (const QString& pin : m_pinned) {
        if (pin.endsWith(QLatin1Char('/')) && name.startsWith(pin)) {
            return true;
        }
    }
    return false;
}

void RefModel::refresh() {
    clear();
    setHorizontalHeaderLabels({QStringLiteral("Reference")});

    m_filterError.clear();
    if (!m_filter.isEmpty() && m_regex) {
        const QRegularExpression re(m_filter);
        if (!re.isValid()) {
            m_filterError = re.errorString();
        }
    }

    appendPinned();
    appendSection(QStringLiteral("Local"), RefKind::LocalBranch);
    appendSection(QStringLiteral("Remote"), RefKind::RemoteBranch);
    appendSection(QStringLiteral("Tags"), RefKind::Tag);
    appendStashes();
    appendWorktrees();
    appendSubmodules();
}

QStandardItem* RefModel::makeSectionItem(const QString& title, Glyph glyph,
                                         const QColor& colour) const {
    auto* item = new QStandardItem(title);
    item->setEditable(false);
    item->setDropEnabled(false);
    item->setData(static_cast<int>(SectionItem), ItemTypeRole);
    item->setIcon(glyphIcon(glyph, colour, kRowIconSide));
    QFont font = item->font();
    font.setBold(true);
    item->setFont(font);
    return item;
}

QStandardItem* RefModel::makeFolderItem(const PathFolder& folder) const {
    auto* item = new QStandardItem(folder.label);
    item->setEditable(false);
    item->setDragEnabled(false);
    item->setDropEnabled(false);
    item->setData(static_cast<int>(FolderItem), ItemTypeRole);
    item->setData(folder.prefix, FolderRole);
    item->setData(m_pinned.contains(folder.prefix), PinnedRole);
    item->setIcon(glyphIcon(Glyph::Folder, theme::textDim(), kRowIconSide));
    item->setToolTip(folder.prefix);
    return item;
}

QStandardItem* RefModel::makeRefItem(const RefInfo& ref, const QString& label) const {
    // The text stays exactly the branch name; HEAD is marked with the font and
    // colour instead, so the name can never be corrupted by decoration.
    const QString name = QString::fromStdString(ref.shortName);
    auto* item = new QStandardItem(label);
    item->setEditable(false);
    item->setDragEnabled(true);
    item->setDropEnabled(true);
    item->setData(QString::fromStdString(ref.fullName), FullNameRole);
    item->setData(QString::fromStdString(ref.target), TargetRole);
    item->setData(static_cast<int>(ref.kind), KindRole);
    item->setData(name, ShortNameRole);
    item->setData(static_cast<int>(RefItem), ItemTypeRole);
    item->setData(prefixOf(name), FolderRole);
    item->setData(m_pinned.contains(name), PinnedRole);
    item->setIcon(glyphIcon(glyphFor(ref.kind), theme::textDim(), kRowIconSide));
    item->setToolTip(QStringLiteral("%1\n%2\n\n%3\nDrag onto another branch to merge or rebase")
                         .arg(QString::fromStdString(ref.fullName),
                              QString::fromStdString(ref.target),
                              name == m_headBranch ? QStringLiteral("current branch")
                                                   : QStringLiteral("double-click to check out")));
    if (!m_headBranch.isEmpty() && name == m_headBranch) {
        QFont font = item->font();
        font.setBold(true);
        item->setFont(font);
    } else if (isGraphHidden(ref)) {
        // A branch the graph is filtering out is drawn dimmed, because the tree
        // and the graph disagreeing about what exists reads as a bug. HEAD is
        // never dimmed: the filter never hides the branch you are on.
        item->setForeground(theme::textDim());
        item->setToolTip(item->toolTip() + QStringLiteral("\n\nHidden from the graph"));
    }
    return item;
}

bool RefModel::isGraphHidden(const RefInfo& ref) const {
    if (ref.kind != RefKind::LocalBranch && ref.kind != RefKind::RemoteBranch) {
        return false; // a tag is never hidden
    }
    const QString name = QString::fromStdString(ref.shortName);
    if (m_solo.isEmpty() && m_hidden.isEmpty()) {
        return false;
    }
    if (!m_solo.isEmpty()) {
        return !m_solo.contains(name);
    }
    return m_hidden.contains(name);
}

void RefModel::appendFolderContents(QStandardItem* parentItem, const PathFolder& folder,
                                    const std::vector<const RefInfo*>& items) const {
    // Folders first, then the references that live directly here — the order every
    // file tree uses, so the shape of the group is readable at a glance.
    for (const PathFolder& child : folder.folders) {
        QStandardItem* item = makeFolderItem(child);
        parentItem->appendRow(item);
        appendFolderContents(item, child, items);
    }
    for (const int index : folder.items) {
        const RefInfo& ref = *items.at(static_cast<std::size_t>(index));
        const QString name = QString::fromStdString(ref.shortName);
        // Inside a folder the row shows the tail of the name; the folder above it
        // already said the rest.
        parentItem->appendRow(makeRefItem(ref, name.mid(folder.prefix.size())));
    }
}

void RefModel::appendSection(const QString& title, RefKind kind) {
    std::vector<const RefInfo*> pending;
    for (const RefInfo& ref : m_refs) {
        if (ref.kind != kind || isMovedToPinned(ref)) {
            continue;
        }
        const QString name = QString::fromStdString(ref.shortName);
        if (matches(name)) {
            pending.push_back(&ref);
        }
    }
    if (pending.empty()) {
        return;
    }
    // Sorted before folding: the tree keeps the order it is given inside each
    // folder, so ordering the names orders every level of it.
    std::sort(pending.begin(), pending.end(), [](const RefInfo* a, const RefInfo* b) {
        return lessNoCase(QString::fromStdString(a->shortName),
                          QString::fromStdString(b->shortName));
    });

    QStringList names;
    std::vector<int> indices;
    names.reserve(static_cast<int>(pending.size()));
    for (std::size_t i = 0; i < pending.size(); ++i) {
        names.push_back(QString::fromStdString(pending.at(i)->shortName));
        indices.push_back(static_cast<int>(i));
    }
    const PathFolder root = buildPathTree(names, indices);

    Glyph glyph = Glyph::Branch;
    if (kind == RefKind::RemoteBranch) {
        glyph = Glyph::Cloud;
    } else if (kind == RefKind::Tag) {
        glyph = Glyph::Tag;
    } else {
        glyph = Glyph::Monitor;
    }

    auto* group = makeSectionItem(title, glyph, theme::textDim());
    appendFolderContents(group, root, pending);
    if (group->rowCount() > 0) {
        appendRow(group);
    } else {
        delete group;
    }
}

void RefModel::appendStashes() {
    if (m_stashMessages.isEmpty()) {
        return;
    }
    auto* group = makeSectionItem(QStringLiteral("Stash"), Glyph::Stash, theme::textDim());
    for (int i = 0; i < m_stashMessages.size(); ++i) {
        const QString label =
            QStringLiteral("stash@{%1}: %2").arg(i).arg(m_stashMessages.at(i));
        auto* item = new QStandardItem(label);
        item->setEditable(false);
        item->setDragEnabled(false);
        item->setDropEnabled(false);
        item->setData(static_cast<int>(StashItem), ItemTypeRole);
        item->setData(i, StashIndexRole);
        item->setData(m_stashOids.value(i), StashOidRole);
        item->setIcon(glyphIcon(Glyph::Stash, theme::textDim(), kRowIconSide));
        item->setToolTip(QStringLiteral("%1\nApply, pop or drop from the context menu")
                             .arg(m_stashOids.value(i)));
        group->appendRow(item);
    }
    appendRow(group);
}

void RefModel::appendWorktrees() {
    // One worktree is the repository itself; the section would only say "you are
    // here". git always lists the main one, so "fewer than two" means none are in use.
    if (m_worktrees.size() < 2) {
        return;
    }
    auto* group = makeSectionItem(QStringLiteral("Worktrees"), Glyph::Worktree, theme::textDim());
    for (const WorktreeEntry& worktree : m_worktrees) {
        const QString path = QString::fromStdString(worktree.path);
        QString label = QString::fromStdString(worktree.name);
        if (!worktree.branch.empty()) {
            QString branch = QString::fromStdString(worktree.branch);
            if (branch.startsWith(QLatin1String("refs/heads/"))) {
                branch = branch.mid(int(std::string("refs/heads/").size()));
            }
            label += QStringLiteral("  ·  %1").arg(branch);
        } else if (worktree.detached) {
            label += QStringLiteral("  ·  detached");
        }
        QStringList marks;
        if (worktree.current) {
            marks << QStringLiteral("current");
        }
        if (worktree.locked) {
            marks << QStringLiteral("locked");
        }
        if (worktree.prunable) {
            marks << QStringLiteral("prunable");
        }
        if (!marks.isEmpty()) {
            label += QStringLiteral("  [%1]").arg(marks.join(QStringLiteral(", ")));
        }

        auto* item = new QStandardItem(label);
        item->setEditable(false);
        item->setDragEnabled(false);
        item->setDropEnabled(false);
        item->setData(static_cast<int>(WorktreeItem), ItemTypeRole);
        item->setData(path, WorktreePathRole);
        item->setData(worktree.current, WorktreeCurrentRole);
        item->setData(worktree.locked, WorktreeLockedRole);
        item->setIcon(glyphIcon(Glyph::Worktree,
                                worktree.current ? theme::accent() : theme::textDim(),
                                kRowIconSide));
        if (worktree.current) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
        QString tip = path + QLatin1Char('\n') +
                      (!worktree.branch.empty() ? QString::fromStdString(worktree.branch)
                                                : QStringLiteral("(detached)"));
        if (worktree.locked && !worktree.lockReason.empty()) {
            tip += QStringLiteral("\nlocked: %1").arg(QString::fromStdString(worktree.lockReason));
        }
        if (worktree.prunable) {
            tip += QStringLiteral("\nprunable: the directory is gone");
        }
        item->setToolTip(tip);
        group->appendRow(item);
    }
    appendRow(group);
}

/// One row per submodule, at its full path: a nested submodule already reads where
/// it lives, so no second tree is built for what the path says.
void RefModel::appendSubmodules() {
    if (m_submodules.empty()) {
        return;
    }
    auto* group =
        makeSectionItem(QStringLiteral("Submodules"), Glyph::Submodule, theme::textDim());
    for (const SubmoduleEntry& submodule : m_submodules) {
        const QString path = QString::fromStdString(submodule.path);
        QString label = path;
        QColor colour = theme::textDim();
        switch (submodule.state) {
        case SubmoduleState::InSync:
            break;
        case SubmoduleState::NotInitialized:
            label += QStringLiteral("  [not initialized]");
            break;
        case SubmoduleState::OutOfSync:
            label += QStringLiteral("  [out of sync]");
            colour = theme::modified();
            break;
        case SubmoduleState::Conflict:
            label += QStringLiteral("  [conflict]");
            colour = theme::danger();
            break;
        }

        auto* item = new QStandardItem(label);
        item->setEditable(false);
        item->setDragEnabled(false);
        item->setDropEnabled(false);
        item->setData(static_cast<int>(SubmoduleItem), ItemTypeRole);
        item->setData(path, SubmodulePathRole);
        item->setData(QString::fromStdString(submodule.url), SubmoduleUrlRole);
        item->setData(static_cast<int>(submodule.state), SubmoduleStateRole);
        item->setIcon(glyphIcon(Glyph::Submodule, colour, kRowIconSide));

        QString tip = path + QLatin1Char('\n') +
                      (submodule.url.empty() ? QStringLiteral("(no URL)")
                                             : QString::fromStdString(submodule.url));
        if (!submodule.branch.empty()) {
            tip += QStringLiteral("\nbranch: %1").arg(QString::fromStdString(submodule.branch));
        }
        tip += QStringLiteral("\nrecorded: %1")
                   .arg(QString::fromStdString(submodule.recordedOid).left(10));
        if (!submodule.checkedOutOid.empty()) {
            tip += QStringLiteral("\nchecked out: %1")
                       .arg(QString::fromStdString(submodule.checkedOutOid).left(10));
        }
        if (!submodule.describe.empty()) {
            tip += QStringLiteral("\n%1").arg(QString::fromStdString(submodule.describe));
        }
        item->setToolTip(tip);
        group->appendRow(item);
    }
    appendRow(group);
}

void RefModel::appendPinned() {
    if (m_pinned.isEmpty()) {
        return;
    }

    auto* group = makeSectionItem(QStringLiteral("Pinned"), Glyph::Paperclip, theme::accent());
    for (const QString& pin : m_pinned) {
        if (pin.endsWith(QLatin1Char('/'))) {
            // A pinned folder brings its whole subtree, from every kind: a folder
            // is a name prefix, and what lives under it is not one section's.
            std::vector<const RefInfo*> pending;
            QStringList names;
            std::vector<int> indices;
            for (const RefInfo& ref : m_refs) {
                const QString name = QString::fromStdString(ref.shortName);
                if (!name.startsWith(pin) || !matches(name)) {
                    continue;
                }
                // Built without the pin's own prefix: the subtree is rooted at it,
                // and keeping the prefix would build the folder inside itself.
                names.push_back(name.mid(pin.size()));
                indices.push_back(static_cast<int>(pending.size()));
                pending.push_back(&ref);
            }
            if (pending.empty()) {
                continue;
            }
            PathFolder shown = buildPathTree(names, indices);
            shown.label = pin.left(pin.size() - 1);
            shown.prefix = pin;

            QStandardItem* item = makeFolderItem(shown);
            group->appendRow(item);
            appendFolderContents(item, shown, pending);
            continue;
        }

        for (const RefInfo& ref : m_refs) {
            const QString name = QString::fromStdString(ref.shortName);
            if (name == pin && matches(name)) {
                // Pinned on its own, with no folder above it, so it keeps its
                // whole name — there is nothing else to say where it lives.
                group->appendRow(makeRefItem(ref, name));
                break;
            }
        }
    }

    if (group->rowCount() > 0) {
        appendRow(group);
    } else {
        delete group;
    }
}

Qt::ItemFlags RefModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags base = QStandardItemModel::flags(index);
    if (!index.isValid()) {
        // The viewport itself: dropping here has no meaning.
        return base;
    }
    if (!isRef(index)) {
        // A section or a folder is not a reference and cannot be a drop target.
        return base & ~Qt::ItemIsDropEnabled;
    }
    return base | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
}

QStringList RefModel::mimeTypes() const {
    return {QString::fromLatin1(kRefMimeType)};
}

QMimeData* RefModel::mimeDataForRef(const RefInfo& ref) {
    const QString payload =
        QStringLiteral("%1|%2|%3|%4")
            .arg(QString::fromStdString(ref.fullName), QString::fromStdString(ref.shortName),
                 QString::fromStdString(ref.target))
            .arg(static_cast<int>(ref.kind));
    auto* data = new QMimeData;
    data->setData(QString::fromLatin1(kRefMimeType), payload.toUtf8());
    data->setText(QString::fromStdString(ref.shortName));
    return data;
}

QMimeData* RefModel::mimeDataForIndex(const QModelIndex& index) {
    if (!isRef(index)) {
        return nullptr; // a section or a folder carries no reference
    }
    // Field by field on purpose: RefInfo's declaration order is not the order of
    // the drag payload, and positional initialisation silently swaps them.
    RefInfo ref;
    ref.fullName = index.data(FullNameRole).toString().toStdString();
    ref.shortName = refShortName(index).toStdString();
    ref.target = index.data(TargetRole).toString().toStdString();
    ref.kind = static_cast<RefKind>(index.data(KindRole).toInt());
    return mimeDataForRef(ref);
}

QMimeData* RefModel::mimeData(const QModelIndexList& indexes) const {
    if (indexes.isEmpty()) {
        return nullptr;
    }
    return mimeDataForIndex(indexes.first());
}

QString RefModel::refShortName(const QModelIndex& index) {
    if (!index.isValid()) {
        return {};
    }
    const QVariant stored = index.data(ShortNameRole);
    return stored.isValid() ? stored.toString() : index.data(Qt::DisplayRole).toString();
}

RefKind RefModel::refKind(const QModelIndex& index) {
    if (!index.isValid()) {
        return RefKind::Other;
    }
    return static_cast<RefKind>(index.data(KindRole).toInt());
}

RefModel::DraggedRef RefModel::decode(const QMimeData* data) {
    DraggedRef ref;
    if (!data || !data->hasFormat(QString::fromLatin1(kRefMimeType))) {
        return ref;
    }
    const QString payload = QString::fromUtf8(data->data(QString::fromLatin1(kRefMimeType)));
    const QStringList parts = payload.split(QLatin1Char('|'));
    if (parts.size() < 4) {
        return ref;
    }
    ref.fullName = parts.at(0);
    ref.shortName = parts.at(1);
    ref.target = parts.at(2);
    ref.kind = static_cast<RefKind>(parts.at(3).toInt());
    return ref;
}

bool RefModel::canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                               const QModelIndex& parent) const {
    Q_UNUSED(action);
    Q_UNUSED(row);
    Q_UNUSED(column);
    if (decode(data).shortName.isEmpty()) {
        return false;
    }
    // Only a drop onto a real reference is meaningful: folders and sections are
    // not things you can merge into.
    return isRef(parent);
}

bool RefModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                            const QModelIndex& parent) {
    Q_UNUSED(action);
    Q_UNUSED(row);
    Q_UNUSED(column);
    if (!canDropMimeData(data, action, row, column, parent)) {
        return false;
    }

    const DraggedRef source = decode(data);
    const QString target = refShortName(parent);
    if (target.isEmpty() || target == source.shortName) {
        return false;
    }

    emit refDroppedOnRef(source, target);
    return true;
}

Qt::DropActions RefModel::supportedDropActions() const {
    return Qt::MoveAction | Qt::CopyAction;
}

} // namespace gk
