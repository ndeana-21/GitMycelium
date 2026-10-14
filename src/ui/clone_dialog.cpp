#include "ui/clone_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace gk {

CloneDialog::CloneDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Clone Repository"));
    setModal(true);
    resize(560, 0);

    auto* layout = new QVBoxLayout(this);

    auto* form = new QFormLayout;
    m_url = new QLineEdit(this);
    m_url->setObjectName(QStringLiteral("cloneUrl"));
    m_url->setPlaceholderText(QStringLiteral("https://host/owner/repo.git"));
    m_url->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("Repository URL"), m_url);

    auto* destRow = new QWidget(this);
    auto* destLayout = new QHBoxLayout(destRow);
    destLayout->setContentsMargins(0, 0, 0, 0);
    m_dest = new QLineEdit(destRow);
    m_dest->setObjectName(QStringLiteral("cloneDestination"));
    m_dest->setPlaceholderText(QStringLiteral("/path/to/clone"));
    m_dest->setClearButtonEnabled(true);
    auto* browse = new QPushButton(QStringLiteral("Browse…"), destRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Clone into"), m_dest->text(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!dir.isEmpty()) {
            m_dest->setText(dir);
        }
    });
    destLayout->addWidget(m_dest, 1);
    destLayout->addWidget(browse);
    form->addRow(QStringLiteral("Clone into"), destRow);

    m_branch = new QLineEdit(this);
    m_branch->setObjectName(QStringLiteral("cloneBranch"));
    m_branch->setPlaceholderText(QStringLiteral("the remote's default branch"));
    form->addRow(QStringLiteral("Branch"), m_branch);

    m_depth = new QSpinBox(this);
    m_depth->setObjectName(QStringLiteral("cloneDepth"));
    m_depth->setRange(0, 100000);
    m_depth->setSpecialValueText(QStringLiteral("Full history"));
    form->addRow(QStringLiteral("Depth"), m_depth);

    m_since = new QLineEdit(this);
    m_since->setObjectName(QStringLiteral("cloneSince"));
    m_since->setPlaceholderText(QStringLiteral("e.g. 2024-01-01 or 6.months.ago"));
    m_since->setClearButtonEnabled(true);
    m_since->setToolTip(QStringLiteral(
        "git clone --shallow-since=<date>: history back to a date. git refuses it "
        "together with a depth, so setting one clears the other."));
    form->addRow(QStringLiteral("Shallow since"), m_since);
    // The two shallow forms cannot be combined; setting one clears the other rather
    // than letting the clone fail on git's "cannot be used together".
    connect(m_since, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!text.trimmed().isEmpty() && m_depth->value() != 0) {
            m_depth->setValue(0);
        }
    });
    connect(m_depth, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (value > 0 && !m_since->text().trimmed().isEmpty()) {
            m_since->clear();
        }
    });

    m_partial = new QCheckBox(QStringLiteral("Partial clone (fetch blobs on demand)"), this);
    m_partial->setObjectName(QStringLiteral("clonePartial"));
    m_partial->setToolTip(QStringLiteral(
        "git clone --filter=blob:none: commits and trees only, which is what makes a very "
        "large repository openable."));
    form->addRow(QString(), m_partial);
    layout->addLayout(form);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    const auto refresh = [this, ok] {
        ok->setEnabled(!m_url->text().trimmed().isEmpty() &&
                       !m_dest->text().trimmed().isEmpty());
    };
    connect(m_url, &QLineEdit::textChanged, this, [refresh] { refresh(); });
    connect(m_dest, &QLineEdit::textChanged, this, [refresh] { refresh(); });
    refresh();

    m_url->setFocus();
}

QString CloneDialog::url() const {
    return m_url->text().trimmed();
}

QString CloneDialog::destination() const {
    return m_dest->text().trimmed();
}

bool CloneDialog::partial() const {
    return m_partial->isChecked();
}

QString CloneDialog::branch() const {
    return m_branch->text().trimmed();
}

int CloneDialog::depth() const {
    return m_depth->value();
}

QString CloneDialog::shallowSince() const {
    return m_since->text().trimmed();
}

} // namespace gk
