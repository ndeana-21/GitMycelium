#include "ui/rebase_editor.h"

#include "theme/icons.h"
#include "theme/theme.h"

#include <QAbstractItemModel>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace gk {
namespace {

enum Column { ActionColumn = 0, CommitColumn = 1 };

constexpr int IdRole = Qt::UserRole;
constexpr int SubjectRole = Qt::UserRole + 1;
constexpr int MessageRole = Qt::UserRole + 1;

/// The action cell is a combo box rather than a run of little buttons per row: the
/// choice belongs to the row, and a combo moves with it when the row is dragged. Using
/// model data (a delegate) rather than a per-row widget is what makes that true.
class ActionDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex&) const override {
        auto* combo = new QComboBox(parent);
        for (RewriteAction action : {RewriteAction::Keep, RewriteAction::Reword,
                                     RewriteAction::Edit, RewriteAction::Squash,
                                     RewriteAction::Fixup, RewriteAction::Drop}) {
            combo->addItem(QString::fromLatin1(rewriteActionName(action)),
                           static_cast<int>(action));
        }
        return combo;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* combo = qobject_cast<QComboBox*>(editor);
        const int value = index.data(Qt::UserRole).toInt();
        const int at = combo->findData(value);
        combo->setCurrentIndex(at < 0 ? 0 : at);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        auto* combo = qobject_cast<QComboBox*>(editor);
        model->setData(index, combo->currentData(), Qt::UserRole);
        model->setData(index, combo->currentText(), Qt::DisplayRole);
    }
};

const char* actionTooltip(RewriteAction action) {
    switch (action) {
    case RewriteAction::Keep: return "replay the commit as it is";
    case RewriteAction::Drop: return "leave the commit out entirely";
    case RewriteAction::Squash: return "fold into the commit above, combining messages";
    case RewriteAction::Fixup: return "fold into the commit above, dropping this message";
    case RewriteAction::Reword: return "replay with a new message";
    case RewriteAction::Edit: return "replay, then stop so it can be amended";
    }
    return "";
}

} // namespace

RebaseEditor::RebaseEditor(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Interactive Rebase"));
    setModal(true);
    setMinimumSize(560, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* hint = new QLabel(
        QStringLiteral("Oldest first. Drag a row to change when it is replayed; "
                       "double-click the action to change what happens to it."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({QStringLiteral("Action"), QStringLiteral("Commit")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setDragDropMode(QAbstractItemView::InternalMove);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setItemDelegateForColumn(ActionColumn, new ActionDelegate(m_tree));
    m_tree->header()->setSectionResizeMode(ActionColumn, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    layout->addWidget(m_tree, 1);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    m_problem = new QLabel(this);
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(QStringLiteral("color: %1;").arg(theme::diffRemovedFg().name()));
    layout->addWidget(m_problem);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_run = new QPushButton(icon(Icon::Check), QStringLiteral("Run Rebase"), this);
    m_run->setDefault(true);
    connect(m_run, &QPushButton::clicked, this, [this] {
        if (!collectMessages()) {
            return;
        }
        refreshValidation();
        if (refusal().isEmpty()) {
            accept();
        }
    });
    buttons->addWidget(cancel);
    buttons->addWidget(m_run);
    layout->addLayout(buttons);

    connect(m_tree, &QTreeWidget::itemChanged, this, [this] { refreshValidation(); });
    connect(m_tree->model(), &QAbstractItemModel::rowsMoved, this,
            [this] { refreshValidation(); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
                const int at = m_tree->indexOfTopLevelItem(item);
                if (at >= 0) {
                    m_tree->editItem(item, ActionColumn);
                }
            });
}

void RebaseEditor::setCandidates(const std::vector<RebaseCandidate>& candidates) {
    rebuild();
    for (const RebaseCandidate& candidate : candidates) {
        auto* item = new QTreeWidgetItem(m_tree);
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled |
                       Qt::ItemIsEditable);
        item->setText(ActionColumn, QString::fromLatin1(rewriteActionName(RewriteAction::Keep)));
        item->setData(ActionColumn, Qt::UserRole, static_cast<int>(RewriteAction::Keep));
        item->setToolTip(ActionColumn, QString::fromLatin1(actionTooltip(RewriteAction::Keep)));
        item->setText(CommitColumn, QStringLiteral("%1  %2")
                                        .arg(QString::fromStdString(candidate.id).left(10),
                                             QString::fromStdString(candidate.subject)));
        item->setData(CommitColumn, IdRole, QString::fromStdString(candidate.id));
        item->setData(CommitColumn, SubjectRole, QString::fromStdString(candidate.subject));
    }
    refreshValidation();
}

void RebaseEditor::rebuild() {
    m_tree->clear();
}

std::vector<RebasePlanRow> RebaseEditor::plan() const {
    std::vector<RebasePlanRow> rows;
    rows.reserve(static_cast<std::size_t>(m_tree->topLevelItemCount()));
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* item = m_tree->topLevelItem(i);
        RebasePlanRow row;
        row.id = item->data(CommitColumn, IdRole).toString().toStdString();
        row.subject = item->data(CommitColumn, SubjectRole).toString().toStdString();
        row.action =
            static_cast<RewriteAction>(item->data(ActionColumn, Qt::UserRole).toInt());
        row.message = item->data(ActionColumn, MessageRole).toString().toStdString();
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<RewriteStep> RebaseEditor::steps() const {
    return rebasePlanSteps(plan());
}

int RebaseEditor::rowCount() const {
    return m_tree->topLevelItemCount();
}

void RebaseEditor::setAction(int row, RewriteAction action) {
    QTreeWidgetItem* item = m_tree->topLevelItem(row);
    if (!item) {
        return;
    }
    item->setText(ActionColumn, QString::fromLatin1(rewriteActionName(action)));
    item->setData(ActionColumn, Qt::UserRole, static_cast<int>(action));
    item->setToolTip(ActionColumn, QString::fromLatin1(actionTooltip(action)));
    refreshValidation();
}

RewriteAction RebaseEditor::actionAt(int row) const {
    const QTreeWidgetItem* item = m_tree->topLevelItem(row);
    return item ? static_cast<RewriteAction>(item->data(ActionColumn, Qt::UserRole).toInt())
                : RewriteAction::Keep;
}

void RebaseEditor::setMessage(int row, const QString& message) {
    if (QTreeWidgetItem* item = m_tree->topLevelItem(row)) {
        item->setData(ActionColumn, MessageRole, message);
    }
    refreshValidation();
}

QString RebaseEditor::messageAt(int row) const {
    const QTreeWidgetItem* item = m_tree->topLevelItem(row);
    return item ? item->data(ActionColumn, MessageRole).toString() : QString();
}

void RebaseEditor::moveRow(int from, int to) {
    if (from < 0 || from >= m_tree->topLevelItemCount() || to < 0 ||
        to >= m_tree->topLevelItemCount() || from == to) {
        return;
    }
    QTreeWidgetItem* item = m_tree->takeTopLevelItem(from);
    m_tree->insertTopLevelItem(to, item);
    m_tree->setCurrentItem(item);
    refreshValidation();
}

QString RebaseEditor::refusal() const {
    return QString::fromStdString(rebasePlanRefusal(plan()));
}

void RebaseEditor::refreshValidation() {
    const QString why = refusal();
    m_problem->setText(why);
    if (m_run) {
        m_run->setEnabled(why.isEmpty());
    }
    if (m_summary) {
        int drops = 0;
        int folds = 0;
        for (const RebasePlanRow& row : plan()) {
            if (row.action == RewriteAction::Drop) {
                ++drops;
            }
            if (row.action == RewriteAction::Squash || row.action == RewriteAction::Fixup) {
                ++folds;
            }
        }
        m_summary->setText(QStringLiteral("%1 commit(s) · %2 dropped · %3 folded")
                               .arg(m_tree->topLevelItemCount())
                               .arg(drops)
                               .arg(folds));
    }
}

bool RebaseEditor::collectMessages() {
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        const auto action =
            static_cast<RewriteAction>(item->data(ActionColumn, Qt::UserRole).toInt());
        if (action != RewriteAction::Reword) {
            continue;
        }
        if (!item->data(ActionColumn, MessageRole).toString().trimmed().isEmpty()) {
            continue;
        }
        bool accepted = false;
        const QString subject = item->data(CommitColumn, SubjectRole).toString();
        const QString message = QInputDialog::getMultiLineText(
            this,
            QStringLiteral("Reword %1")
                .arg(item->data(CommitColumn, IdRole).toString().left(10)),
            QStringLiteral("The new commit message:"), subject, &accepted);
        if (!accepted) {
            return false;
        }
        item->setData(ActionColumn, MessageRole, message);
    }
    return true;
}

} // namespace gk
