#include "ui/submodule_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace gk {

SubmoduleDialog::SubmoduleDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Add submodule"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(QStringLiteral("Repository URL:"), this));
    m_url = new QLineEdit(this);
    m_url->setPlaceholderText(QStringLiteral("https://example.com/lib.git"));
    layout->addWidget(m_url);

    layout->addWidget(new QLabel(QStringLiteral("Path (optional):"), this));
    m_path = new QLineEdit(this);
    m_path->setPlaceholderText(QStringLiteral("lib"));
    layout->addWidget(m_path);

    layout->addWidget(new QLabel(QStringLiteral("Branch to track (optional):"), this));
    m_branch = new QLineEdit(this);
    m_branch->setPlaceholderText(QStringLiteral("main"));
    layout->addWidget(m_branch);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString SubmoduleDialog::url() const {
    return m_url->text().trimmed();
}

QString SubmoduleDialog::path() const {
    return m_path->text().trimmed();
}

QString SubmoduleDialog::branch() const {
    return m_branch->text().trimmed();
}

} // namespace gk
