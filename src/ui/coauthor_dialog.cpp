#include "ui/coauthor_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {

namespace {

QString describe(const CoAuthor& author) {
    return QStringLiteral("%1 <%2>")
        .arg(QString::fromStdString(author.name), QString::fromStdString(author.email));
}

} // namespace

CoAuthorDialog::CoAuthorDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Co-authors"));
    setModal(true);
    resize(460, 320);

    auto* layout = new QVBoxLayout(this);

    auto* recentRow = new QHBoxLayout;
    recentRow->addWidget(new QLabel(QStringLiteral("Recent"), this));
    m_recent = new QComboBox(this);
    m_recent->setObjectName(QStringLiteral("coauthorRecent"));
    m_recent->setMinimumWidth(220);
    recentRow->addWidget(m_recent, 1);
    layout->addLayout(recentRow);

    auto* form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("coauthorName"));
    m_name->setPlaceholderText(QStringLiteral("Ada Lovelace"));
    m_email = new QLineEdit(this);
    m_email->setObjectName(QStringLiteral("coauthorEmail"));
    m_email->setPlaceholderText(QStringLiteral("ada@example.com"));
    form->addRow(QStringLiteral("Name"), m_name);
    form->addRow(QStringLiteral("Email"), m_email);
    layout->addLayout(form);

    m_add = new QPushButton(QStringLiteral("Add"), this);
    m_add->setObjectName(QStringLiteral("coauthorAdd"));
    layout->addWidget(m_add);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("coauthorList"));
    layout->addWidget(m_list, 1);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_add, &QPushButton::clicked, this, &CoAuthorDialog::addCurrent);
    connect(m_name, &QLineEdit::textChanged, this, &CoAuthorDialog::refreshOkState);
    connect(m_email, &QLineEdit::textChanged, this, &CoAuthorDialog::refreshOkState);
    connect(m_recent, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= m_suggestions.size()) {
            return;
        }
        m_name->setText(QString::fromStdString(m_suggestions[index].name));
        m_email->setText(QString::fromStdString(m_suggestions[index].email));
    });

    refreshOkState();
    m_name->setFocus();
}

void CoAuthorDialog::setSuggestions(const std::vector<CoAuthor>& authors) {
    m_suggestions = authors;
    m_recent->clear();
    for (const CoAuthor& author : m_suggestions) {
        m_recent->addItem(describe(author));
    }
    m_recent->setEnabled(!m_suggestions.empty());
}

std::vector<CoAuthor> CoAuthorDialog::authors() const {
    return m_authors;
}

void CoAuthorDialog::addCurrent() {
    const QString name = m_name->text().trimmed();
    const QString email = m_email->text().trimmed();
    if (name.isEmpty() || email.isEmpty()) {
        return;
    }
    CoAuthor author{name.toStdString(), email.toStdString()};
    // A person added twice is one trailer; git would deduplicate it anyway, but the
    // list should not lie about what it will produce.
    for (const CoAuthor& existing : m_authors) {
        if (existing.email == author.email) {
            return;
        }
    }
    m_authors.push_back(std::move(author));
    m_list->addItem(describe(m_authors.back()));
    m_name->clear();
    m_email->clear();
    refreshOkState();
    m_name->setFocus();
}

void CoAuthorDialog::refreshOkState() {
    // OK only means something with at least one person and no half-typed row: a
    // dialog that accepts a blank co-author writes a trailer nobody asked for.
    const bool ready =
        !m_authors.empty() && m_name->text().trimmed().isEmpty() && m_email->text().trimmed().isEmpty();
    m_ok->setEnabled(ready);
}

} // namespace gk
