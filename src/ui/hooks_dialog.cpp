#include "ui/hooks_dialog.h"

#include "theme/theme.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace gk {

HooksDialog::HooksDialog(const std::vector<HookEntry>& hooks, const QHash<QString, QString>& contents,
                         const QString& hooksPath, QWidget* parent)
    : QDialog(parent), m_contents(contents), m_hooksPath(hooksPath) {
    setWindowTitle(QStringLiteral("Git hooks"));
    setModal(true);
    resize(720, 520);

    auto* layout = new QVBoxLayout(this);

    auto* note = new QLabel(
        QStringLiteral("A hook runs when git performs the matching step. The switch is the "
                       "executable bit; a hook git ships as a sample stays inert until adopted."),
        this);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("hooksTree"));
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({QStringLiteral("Hook"), QStringLiteral("State")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    for (const HookEntry& hook : hooks) {
        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::fromStdString(hook.name));
        item->setData(0, Qt::UserRole, QString::fromStdString(hook.name));
        QString state = QString::fromStdString(hookStateName(hook.state));
        if (hook.serverSide) {
            state += QStringLiteral(" (server-side)");
            item->setToolTip(0, QStringLiteral("Runs on the remote, not in this repository"));
            // A QTreeWidgetItem is checkable by default; a server-side hook must not
            // offer a switch this repository cannot honour.
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        } else {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, hook.state == HookState::Enabled ? Qt::Checked : Qt::Unchecked);
        }
        if (!hook.path.empty()) {
            item->setToolTip(1, QString::fromStdString(hook.path));
        }
        item->setText(1, state);
    }
    layout->addWidget(m_tree, 1);

    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName(QStringLiteral("hookContent"));
    m_editor->setReadOnly(true);
    m_editor->setFont(theme::monoFont());
    m_editor->setPlaceholderText(QStringLiteral("Select a hook to see its content."));
    layout->addWidget(m_editor, 1);

    auto* buttons = new QHBoxLayout();
    m_editButton = new QPushButton(QStringLiteral("Edit"), this);
    m_saveButton = new QPushButton(QStringLiteral("Save content"), this);
    m_saveButton->setEnabled(false);
    auto* adopt = new QPushButton(QStringLiteral("Create from sample"), this);
    auto* folder = new QPushButton(QStringLiteral("Open hooks folder"), this);
    buttons->addWidget(m_editButton);
    buttons->addWidget(m_saveButton);
    buttons->addWidget(adopt);
    buttons->addStretch(1);
    buttons->addWidget(folder);
    layout->addLayout(buttons);

    auto* dialogButtons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(dialogButtons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(dialogButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(dialogButtons);

    connect(m_tree, &QTreeWidget::currentItemChanged, this, &HooksDialog::showSelectedContent);
    connect(m_editButton, &QPushButton::clicked, this, [this] {
        const bool editing = m_editor->isReadOnly();
        m_editor->setReadOnly(!editing);
        m_saveButton->setEnabled(editing);
        m_editButton->setText(editing ? QStringLiteral("Stop editing") : QStringLiteral("Edit"));
    });
    connect(m_saveButton, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem* item = m_tree->currentItem();
        if (!item) {
            return;
        }
        const QString name = item->data(0, Qt::UserRole).toString();
        const QString text = m_editor->toPlainText();
        if (text == m_contents.value(name)) {
            m_edited.remove(name);
        } else {
            m_edited.insert(name, text);
        }
    });
    connect(adopt, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem* item = m_tree->currentItem();
        if (item && item->flags().testFlag(Qt::ItemIsUserCheckable)) {
            item->setCheckState(0, Qt::Checked);
        }
    });
    connect(folder, &QPushButton::clicked, this, &HooksDialog::openHooksFolderRequested);

    if (m_tree->topLevelItemCount() > 0) {
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    }
}

QTreeWidgetItem* HooksDialog::itemFor(const QString& name) const {
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == name) {
            return item;
        }
    }
    return nullptr;
}

QStringList HooksDialog::enabledNames() const {
    QStringList out;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        if (!item->flags().testFlag(Qt::ItemIsUserCheckable)) {
            continue;
        }
        if (item->checkState(0) == Qt::Checked) {
            out << item->data(0, Qt::UserRole).toString();
        }
    }
    return out;
}

QStringList HooksDialog::knownNames() const {
    QStringList out;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        out << m_tree->topLevelItem(i)->data(0, Qt::UserRole).toString();
    }
    return out;
}

void HooksDialog::setHookEnabled(const QString& name, bool enabled) {
    if (QTreeWidgetItem* item = itemFor(name)) {
        if (item->flags().testFlag(Qt::ItemIsUserCheckable)) {
            item->setCheckState(0, enabled ? Qt::Checked : Qt::Unchecked);
        }
    }
}

bool HooksDialog::hookEnabled(const QString& name) const {
    if (QTreeWidgetItem* item = itemFor(name)) {
        return item->flags().testFlag(Qt::ItemIsUserCheckable) &&
               item->checkState(0) == Qt::Checked;
    }
    return false;
}

void HooksDialog::showSelectedContent() {
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item) {
        m_editor->clear();
        return;
    }
    const QString name = item->data(0, Qt::UserRole).toString();
    const QString text = m_edited.contains(name) ? m_edited.value(name) : m_contents.value(name);
    m_editor->setPlainText(text);
    m_editor->setReadOnly(true);
    m_saveButton->setEnabled(false);
    m_editButton->setText(QStringLiteral("Edit"));
}

} // namespace gk
