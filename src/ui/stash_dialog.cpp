#include "ui/stash_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace gk {

StashDialog::StashDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Stash working changes"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("Message (optional):"), this));

    m_message = new QLineEdit(this);
    m_message->setPlaceholderText(QStringLiteral("Saved by GitMycelium"));
    layout->addWidget(m_message);

    m_untracked = new QCheckBox(QStringLiteral("Include untracked files"), this);
    m_untracked->setToolTip(
        QStringLiteral("Also stash files git does not track yet (git stash --include-untracked)"));
    layout->addWidget(m_untracked);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString StashDialog::message() const {
    return m_message->text();
}

bool StashDialog::includeUntracked() const {
    return m_untracked->isChecked();
}

} // namespace gk
