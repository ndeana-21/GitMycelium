#include "model/commit_files_model.h"

#include "model/path_tree.h"
#include "theme/glyphs.h"
#include "theme/icons.h"
#include "theme/theme.h"

#include <QFont>
#include <QIcon>
#include <QStringList>

#include <algorithm>
#include <iterator>

namespace gk {
namespace {

constexpr int kRowIconSide = 14;

/// A file change says four things, so it needs four glyphs: a single "changed"
/// marker forces the user to open the diff to find out which.
struct StatusLook {
    Glyph glyph;
    QColor colour;
    QString label;
};

StatusLook lookFor(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::Added:
    case ChangeKind::Untracked:
        return {Glyph::Plus, theme::diffAddedFg(), QStringLiteral("added")};
    case ChangeKind::Deleted:
        return {Glyph::Minus, theme::diffRemovedFg(), QStringLiteral("deleted")};
    case ChangeKind::Renamed:
        return {Glyph::Arrow, theme::accentAlt(), QStringLiteral("renamed")};
    case ChangeKind::TypeChange:
        return {Glyph::Arrow, theme::accentAlt(), QStringLiteral("type changed")};
    case ChangeKind::Conflicted:
        return {Glyph::Arrow, theme::danger(), QStringLiteral("conflicted")};
    case ChangeKind::Modified:
        break;
    }
    return {Glyph::Pencil, theme::modified(), QStringLiteral("modified")};
}

QString statusName(ChangeKind kind) {
    return lookFor(kind).label;
}

} // namespace

CommitFilesModel::CommitFilesModel(QObject* parent) : QStandardItemModel(parent) {
    setHorizontalHeaderLabels({QStringLiteral("File")});
}

void CommitFilesModel::setFiles(const std::vector<ChangedFile>& files) {
    m_files = files;
    m_staged.assign(m_files.size(), false);
    m_submodule.assign(m_files.size(), false);
    m_submoduleTip.assign(m_files.size(), QString());
    m_workingTree = false;
    m_grouped = false;
    rebuild();
}

void CommitFilesModel::setWorkingTreeSide(const std::vector<FileChange>& changes, bool staged) {
    std::vector<FileChange> side;
    std::copy_if(changes.begin(), changes.end(), std::back_inserter(side),
                 [staged](const FileChange& change) { return change.staged == staged; });
    assignWorkingTree(side);
    m_grouped = false;
    rebuild();
}

void CommitFilesModel::setWorkingTree(const std::vector<FileChange>& changes) {
    assignWorkingTree(changes);
    m_grouped = true;
    rebuild();
}

void CommitFilesModel::assignWorkingTree(const std::vector<FileChange>& changes) {
    m_files.clear();
    m_staged.clear();
    m_submodule.clear();
    m_submoduleTip.clear();
    m_files.reserve(changes.size());
    for (const FileChange& change : changes) {
        ChangedFile file;
        file.path = change.path;
        file.kind = change.kind;
        m_files.push_back(file);
        m_staged.push_back(change.staged);
        m_submodule.push_back(change.submodule);
        if (!change.submodule) {
            m_submoduleTip.push_back(QString());
            continue;
        }
        const QString recorded = QString::fromStdString(change.recordedOid).left(10);
        const QString checkedOut = QString::fromStdString(change.checkedOutOid).left(10);
        m_submoduleTip.push_back(
            checkedOut.isEmpty()
                ? QStringLiteral("submodule pointer — recorded %1, not initialized")
                      .arg(recorded.isEmpty() ? QStringLiteral("(unknown)") : recorded)
                : QStringLiteral("submodule pointer — recorded %1, checked out %2")
                      .arg(recorded.isEmpty() ? QStringLiteral("(unknown)") : recorded,
                           checkedOut));
    }
    m_workingTree = true;
}

void CommitFilesModel::setFolderFilter(const QString& prefix) {
    if (m_folderFilter == prefix) {
        return;
    }
    m_folderFilter = prefix;
    rebuild();
}

void CommitFilesModel::setMode(Mode mode) {
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    rebuild();
}

void CommitFilesModel::setSort(Sort sort) {
    if (m_sort == sort) {
        return;
    }
    m_sort = sort;
    rebuild();
}

int CommitFilesModel::countOf(ChangeKind kind) const {
    return static_cast<int>(std::count_if(m_files.begin(), m_files.end(),
                                          [kind](const ChangedFile& f) { return f.kind == kind; }));
}

bool CommitFilesModel::hasUnstaged() const {
    if (!m_workingTree) {
        return false;
    }
    for (std::size_t i = 0; i < m_staged.size(); ++i) {
        if (!m_staged[i]) {
            return true;
        }
    }
    return false;
}

bool CommitFilesModel::isFile(const QModelIndex& index) {
    return index.isValid() && index.data(ItemTypeRole).toInt() == FileItem;
}

QString CommitFilesModel::pathOf(const QModelIndex& index) {
    return isFile(index) ? index.data(PathRole).toString() : QString();
}

ChangeKind CommitFilesModel::statusOf(const QModelIndex& index) {
    if (!index.isValid()) {
        return ChangeKind::Modified;
    }
    return static_cast<ChangeKind>(index.data(StatusRole).toInt());
}

bool CommitFilesModel::isStaged(const QModelIndex& index) {
    return index.isValid() && index.data(StagedRole).toBool();
}

void CommitFilesModel::rebuild() {
    clear();
    setHorizontalHeaderLabels({QStringLiteral("File")});

    // Order first, shape second: the folder tree keeps the order it is given, so
    // sorting the files sorts every level of the tree as well.
    std::vector<int> order;
    order.reserve(m_files.size());
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        // A folder filter narrows the list before it is ordered or folded, so both
        // shapes obey it and neither needs a second code path.
        if (!m_folderFilter.isEmpty() &&
            !QString::fromStdString(m_files.at(i).path).startsWith(m_folderFilter)) {
            continue;
        }
        order.push_back(static_cast<int>(i));
    }
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        const ChangedFile& left = m_files.at(static_cast<std::size_t>(a));
        const ChangedFile& right = m_files.at(static_cast<std::size_t>(b));
        const QString leftPath = QString::fromStdString(left.path);
        const QString rightPath = QString::fromStdString(right.path);
        switch (m_sort) {
        case PathDescending:
            return QString::compare(leftPath, rightPath, Qt::CaseInsensitive) > 0;
        case ByStatus:
            if (left.kind != right.kind) {
                return static_cast<int>(left.kind) < static_cast<int>(right.kind);
            }
            break;
        case PathAscending:
            break;
        }
        return QString::compare(leftPath, rightPath, Qt::CaseInsensitive) < 0;
    });

    const auto makeFileItem = [this](int index) {
        const ChangedFile& file = m_files.at(static_cast<std::size_t>(index));
        const bool submodule = m_submodule.at(static_cast<std::size_t>(index));
        // A submodule row is a pointer, not a file: the submodule glyph and the two
        // oids are what tell the reader that staging it records a commit, not text.
        const StatusLook look =
            submodule ? StatusLook{Glyph::Submodule, theme::accentAlt(),
                                   QStringLiteral("submodule pointer")}
                      : lookFor(file.kind);
        const QString path = QString::fromStdString(file.path);

        // In the tree the folder above the row already carries the rest of the
        // path; in the flat list there is no folder, so the row must carry it all.
        const QString label = (m_mode == TreeMode && !m_grouped)
                                  ? path.section(QLatin1Char('/'), -1)
                                  : path;
        auto* item = new QStandardItem(label);
        item->setEditable(false);
        item->setData(path, PathRole);
        item->setData(static_cast<int>(file.kind), StatusRole);
        item->setData(static_cast<int>(FileItem), ItemTypeRole);
        item->setData(static_cast<bool>(m_staged.at(static_cast<std::size_t>(index))), StagedRole);
        item->setData(submodule, SubmoduleRole);
        if (submodule) {
            item->setData(m_submoduleTip.at(static_cast<std::size_t>(index)), SubmoduleTipRole);
        }
        item->setData(path.section(QLatin1Char('/'), 0, -2) +
                          (path.contains(QLatin1Char('/')) ? QStringLiteral("/") : QString()),
                      PrefixRole);
        item->setIcon(glyphIcon(look.glyph, look.colour, kRowIconSide));

        QString tip = submodule
                          ? QStringLiteral("%1\n%2")
                                .arg(path, m_submoduleTip.at(static_cast<std::size_t>(index)))
                          : QStringLiteral("%1\n%2").arg(path, statusName(file.kind));
        if (!file.oldPath.empty() && file.oldPath != file.path) {
            tip += QStringLiteral("\nfrom %1").arg(QString::fromStdString(file.oldPath));
        }
        item->setToolTip(tip);
        if (file.kind == ChangeKind::Deleted) {
            // A file that is gone reads as gone even before the icon is noticed.
            item->setForeground(theme::textDim());
        }
        return item;
    };

    if (m_grouped) {
        for (const bool staged : {true, false}) {
            const QString title = staged ? QStringLiteral("Staged") : QStringLiteral("Unstaged");
            auto* group = new QStandardItem(title);
            group->setEditable(false);
            group->setData(static_cast<int>(GroupItem), ItemTypeRole);
            QFont bold = group->font();
            bold.setBold(true);
            group->setFont(bold);
            group->setIcon(icon(staged ? Icon::Success : Icon::FileModified,
                                staged ? theme::success() : theme::textDim(), kRowIconSide));
            for (const int index : order) {
                if (m_staged.at(static_cast<std::size_t>(index)) == staged) {
                    group->appendRow(makeFileItem(index));
                }
            }
            group->setText(QStringLiteral("%1 (%2)").arg(title).arg(group->rowCount()));
            appendRow(group);
        }
        return;
    }

    if (m_mode == PathMode) {
        for (const int index : order) {
            appendRow(makeFileItem(index));
        }
        return;
    }

    QStringList names;
    names.reserve(static_cast<int>(order.size()));
    for (const int index : order) {
        names.push_back(QString::fromStdString(m_files.at(static_cast<std::size_t>(index)).path));
    }
    const PathFolder root = buildPathTree(names, order);

    // A folder row: the same folder glyph the branch tree uses, so "folder" means
    // one thing in this window.
    const std::function<void(QStandardItem*, const PathFolder&)> fill =
        [&](QStandardItem* parent, const PathFolder& folder) {
            for (const PathFolder& child : folder.folders) {
                auto* item = new QStandardItem(child.label);
                item->setEditable(false);
                item->setData(static_cast<int>(FolderItem), ItemTypeRole);
                item->setData(child.prefix, PrefixRole);
                item->setIcon(glyphIcon(Glyph::Folder, theme::textDim(), kRowIconSide));
                item->setToolTip(child.prefix);
                parent->appendRow(item);
                fill(item, child);
            }
            for (const int index : folder.items) {
                parent->appendRow(makeFileItem(index));
            }
        };
    fill(invisibleRootItem(), root);
}

} // namespace gk
