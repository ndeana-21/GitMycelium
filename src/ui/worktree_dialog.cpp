#include "ui/worktree_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {

WorktreeDialog::WorktreeDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("New worktree"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(QStringLiteral("Directory:"), this));
    auto* pathRow = new QHBoxLayout();
    m_path = new QLineEdit(this);
    m_path->setPlaceholderText(QStringLiteral("/path/to/second-checkout"));
    pathRow->addWidget(m_path, 1);
    auto* browse = new QPushButton(QStringLiteral("Browse…"), this);
    pathRow->addWidget(browse);
    layout->addLayout(pathRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose an empty directory for the worktree"));
        if (!dir.isEmpty()) {
            m_path->setText(dir);
        }
    });

    layout->addWidget(new QLabel(QStringLiteral("Branch:"), this));
    m_branch = new QComboBox(this);
    m_branch->setEditable(true);
    m_branch->setInsertPolicy(QComboBox::NoInsert);
    layout->addWidget(m_branch);

    m_create = new QCheckBox(QStringLiteral("Create a new branch"), this);
    m_create->setChecked(true);
    layout->addWidget(m_create);

    layout->addWidget(new QLabel(QStringLiteral("Start point (new branch only):"), this));
    m_start = new QLineEdit(this);
    m_start->setPlaceholderText(QStringLiteral("HEAD"));
    layout->addWidget(m_start);

    connect(m_create, &QCheckBox::toggled, m_start, &QWidget::setEnabled);
    m_start->setEnabled(m_create->isChecked());

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void WorktreeDialog::setBranches(const QStringList& branches) {
    m_branch->addItems(branches);
}

QString WorktreeDialog::path() const {
    return m_path->text().trimmed();
}

QString WorktreeDialog::branch() const {
    return m_branch->currentText().trimmed();
}

bool WorktreeDialog::createBranch() const {
    return m_create->isChecked();
}

QString WorktreeDialog::startPoint() const {
    return m_start->text().trimmed();
}

} // namespace gk
