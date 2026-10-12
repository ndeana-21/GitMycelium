#include "ui/sparse_checkout_dialog.h"

#include "theme/theme.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {
namespace {

/// A cone entry matches a top-level directory when it is that directory or
/// something under it (`src/app` keeps `src` too, because cone mode expands the
/// parent chain).
bool coneKeeps(const QStringList& inCone, const QString& directory) {
    for (const QString& entry : inCone) {
        if (entry == directory || entry.startsWith(directory + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

} // namespace

SparseCheckoutDialog::SparseCheckoutDialog(const QStringList& directories, const QStringList& inCone,
                                           bool enabled, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(enabled ? QStringLiteral("Sparse Checkout — on")
                           : QStringLiteral("Sparse Checkout — off"));
    setMinimumSize(460, 420);
    setStyleSheet(theme::styleSheet());

    auto* root = new QVBoxLayout(this);
    auto* hint = new QLabel(
        QStringLiteral("Only the checked directories stay on disk. Their files are still in "
                       "HEAD, and unchecking one removes it from the working tree."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim().name()));
    root->addWidget(hint);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    for (const QString& directory : directories) {
        auto* item = new QListWidgetItem(directory, m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(!enabled || coneKeeps(inCone, directory) ? Qt::Checked
                                                                     : Qt::Unchecked);
    }
    root->addWidget(m_list, 1);

    if (m_list->count() == 0) {
        auto* empty = new QLabel(QStringLiteral("This repository has no directories to choose."),
                                 this);
        empty->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim().name()));
        root->addWidget(empty);
    }

    auto* buttons = new QHBoxLayout;
    auto* apply = new QPushButton(QStringLiteral("Apply"), this);
    apply->setEnabled(m_list->count() > 0);
    auto* disable = new QPushButton(QStringLiteral("Disable (check out everything)"), this);
    disable->setEnabled(enabled);
    buttons->addWidget(apply);
    buttons->addWidget(disable);
    buttons->addStretch(1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->addWidget(close);
    root->addLayout(buttons);

    connect(apply, &QPushButton::clicked, this, [this] {
        m_action = Action::Apply;
        accept();
    });
    connect(disable, &QPushButton::clicked, this, [this] {
        m_action = Action::Disable;
        accept();
    });
}

QStringList SparseCheckoutDialog::chosenDirectories() const {
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem* item = m_list->item(i);
        if (item->checkState() == Qt::Checked) {
            out << item->text();
        }
    }
    return out;
}

} // namespace gk
