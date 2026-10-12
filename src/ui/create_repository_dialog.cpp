#include "ui/create_repository_dialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {

CreateRepositoryDialog::CreateRepositoryDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Create Repository"));
    setObjectName(QStringLiteral("createRepositoryDialog"));

    auto* layout = new QVBoxLayout(this);

    auto* form = new QFormLayout;
    m_path = new QLineEdit(this);
    m_path->setObjectName(QStringLiteral("createRepoPath"));
    m_path->setPlaceholderText(QStringLiteral("/path/to/new-repository"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), this);
    browse->setObjectName(QStringLiteral("createRepoBrowse"));
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browse);
    form->addRow(QStringLiteral("Directory"), pathRow);

    m_branch = new QLineEdit(QStringLiteral("main"), this);
    m_branch->setObjectName(QStringLiteral("createRepoBranch"));
    form->addRow(QStringLiteral("Initial branch"), m_branch);
    layout->addLayout(form);

    auto* hint = new QLabel(
        QStringLiteral("The directory is created if it does not exist."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose a directory"), m_path->text());
        if (!directory.isEmpty()) {
            m_path->setText(directory);
        }
    });
}

QString CreateRepositoryDialog::path() const {
    return m_path->text().trimmed();
}

QString CreateRepositoryDialog::initialBranch() const {
    const QString branch = m_branch->text().trimmed();
    return branch.isEmpty() ? QStringLiteral("main") : branch;
}

} // namespace gk
