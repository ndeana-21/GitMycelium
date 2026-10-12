#include "ui/profile_dialog.h"

#include "theme/theme.h"
#include "ui/profiles.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {

ProfileDialog::ProfileDialog(const QString& activeName, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Identity Profiles"));
    setMinimumSize(520, 480);
    setStyleSheet(theme::styleSheet());

    auto* root = new QVBoxLayout(this);
    auto* hint = new QLabel(
        QStringLiteral("A profile is a name, an email and an optional signing key. Applying one "
                       "writes it into this repository's local config, so it changes only what "
                       "this checkout commits as."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim().name()));
    root->addWidget(hint);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(m_list, 1);

    // A nested form: a second layout on the dialog itself would reparent wrong.
    auto* form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("Work"));
    m_user = new QLineEdit(this);
    m_user->setPlaceholderText(QStringLiteral("Ada Lovelace"));
    m_email = new QLineEdit(this);
    m_email->setPlaceholderText(QStringLiteral("ada@example.com"));
    m_key = new QLineEdit(this);
    m_key->setPlaceholderText(QStringLiteral("optional key id or path"));
    m_sign = new QCheckBox(QStringLiteral("Sign commits with this key"), this);
    form->addRow(QStringLiteral("Profile name"), m_name);
    form->addRow(QStringLiteral("user.name"), m_user);
    form->addRow(QStringLiteral("user.email"), m_email);
    form->addRow(QStringLiteral("Signing key"), m_key);
    form->addRow(QString(), m_sign);
    root->addLayout(form);

    auto* buttons = new QHBoxLayout;
    auto* newButton = new QPushButton(QStringLiteral("New"), this);
    auto* saveButton = new QPushButton(QStringLiteral("Save"), this);
    auto* removeButton = new QPushButton(QStringLiteral("Remove"), this);
    auto* useButton = new QPushButton(QStringLiteral("Use for this repository"), this);
    buttons->addWidget(newButton);
    buttons->addWidget(saveButton);
    buttons->addWidget(removeButton);
    buttons->addStretch(1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->addWidget(close);
    root->addLayout(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) {
        loadSelectionIntoFields();
    });
    connect(newButton, &QPushButton::clicked, this, [this] {
        m_list->setCurrentRow(-1);
        m_name->clear();
        m_user->clear();
        m_email->clear();
        m_key->clear();
        m_sign->setChecked(false);
        m_name->setFocus();
    });
    connect(saveButton, &QPushButton::clicked, this, [this] { saveFields(); });
    connect(removeButton, &QPushButton::clicked, this, [this] {
        const QListWidgetItem* item = m_list->currentItem();
        if (!item) {
            return;
        }
        const QString name = item->text();
        profiles::remove(name);
        reloadList(QString());
    });
    connect(useButton, &QPushButton::clicked, this, [this] {
        // Save what is on screen first, so "Use" never applies a stale definition.
        saveFields();
        if (m_name->text().trimmed().isEmpty()) {
            return;
        }
        m_chosen = m_name->text().trimmed();
        accept();
    });

    reloadList(activeName);
}

void ProfileDialog::reloadList(const QString& select) {
    m_list->clear();
    const QStringList known = profiles::names();
    for (const QString& name : known) {
        m_list->addItem(name);
    }
    const int row = known.indexOf(select);
    if (row >= 0) {
        m_list->setCurrentRow(row);
    } else if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
}

void ProfileDialog::loadSelectionIntoFields() {
    const QListWidgetItem* item = m_list->currentItem();
    if (!item) {
        return;
    }
    const GitProfile profile = profiles::load(item->text());
    m_name->setText(profile.name);
    m_user->setText(profile.userName);
    m_email->setText(profile.email);
    m_key->setText(profile.signingKey);
    m_sign->setChecked(profile.sign);
}

void ProfileDialog::saveFields() {
    const QString name = m_name->text().trimmed();
    if (name.isEmpty()) {
        return;
    }
    GitProfile profile;
    profile.name = name;
    profile.userName = m_user->text().trimmed();
    profile.email = m_email->text().trimmed();
    profile.signingKey = m_key->text().trimmed();
    profile.sign = m_sign->isChecked();
    profiles::save(profile);
    reloadList(name);
}

} // namespace gk
