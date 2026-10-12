#include "ui/reflog_dialog.h"

#include "theme/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace gk {

ReflogDialog::ReflogDialog(const std::vector<ReflogEntry>& entries, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Reflog — %1 entr(y/ies)").arg(entries.size()));
    setMinimumSize(720, 460);
    setStyleSheet(theme::styleSheet());

    auto* root = new QVBoxLayout(this);
    auto* hint = new QLabel(
        QStringLiteral("Every move a ref has made. Check one out detached, or start a branch "
                       "from it."),
        this);
    hint->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim().name()));
    root->addWidget(hint);

    m_list = new QTreeWidget(this);
    m_list->setRootIsDecorated(false);
    m_list->setAllColumnsShowFocus(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHeaderLabels({QStringLiteral("Selector"), QStringLiteral("Action"),
                             QStringLiteral("Message"), QStringLiteral("Commit"),
                             QStringLiteral("Author")});
    for (const ReflogEntry& entry : entries) {
        auto* item = new QTreeWidgetItem(m_list);
        item->setText(0, QString::fromStdString(entry.selector));
        item->setText(1, QString::fromStdString(entry.action));
        item->setText(2, QString::fromStdString(entry.message));
        item->setText(3, QString::fromStdString(entry.shortId));
        item->setText(4, QString::fromStdString(entry.author));
        item->setData(0, Qt::UserRole, QString::fromStdString(entry.id));
        if (entry.action.empty() && !entry.message.empty()) {
            // git wrote no "verb: " prefix; show the subject in the action column too so
            // the row is not half empty.
            item->setText(1, QString::fromStdString(entry.message));
        }
    }
    m_list->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    root->addWidget(m_list, 1);

    auto* buttons = new QHBoxLayout;
    m_checkoutButton = new QPushButton(QStringLiteral("Check out (detached)"), this);
    m_branchButton = new QPushButton(QStringLiteral("Create branch here…"), this);
    auto* copyButton = new QPushButton(QStringLiteral("Copy SHA"), this);
    buttons->addWidget(m_checkoutButton);
    buttons->addWidget(m_branchButton);
    buttons->addWidget(copyButton);
    buttons->addStretch(1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->addWidget(close);
    root->addLayout(buttons);

    const auto enableForSelection = [this] {
        const bool has = !currentCommit().isEmpty();
        m_checkoutButton->setEnabled(has);
        m_branchButton->setEnabled(has);
    };
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, enableForSelection);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { choose(QStringLiteral("checkout")); });
    connect(m_checkoutButton, &QPushButton::clicked, this, [this] { choose(QStringLiteral("checkout")); });
    connect(m_branchButton, &QPushButton::clicked, this, [this] { choose(QStringLiteral("branch")); });
    connect(copyButton, &QPushButton::clicked, this, [this] {
        const QString commit = currentCommit();
        if (!commit.isEmpty()) {
            QApplication::clipboard()->setText(commit);
        }
    });

    if (m_list->topLevelItemCount() > 0) {
        m_list->setCurrentItem(m_list->topLevelItem(0));
    }
    enableForSelection();
}

QString ReflogDialog::currentCommit() const {
    const QTreeWidgetItem* item = m_list ? m_list->currentItem() : nullptr;
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void ReflogDialog::choose(const QString& action) {
    const QString commit = currentCommit();
    if (commit.isEmpty()) {
        return;
    }
    m_chosenCommit = commit;
    m_chosenAction = action;
    accept();
}

} // namespace gk
