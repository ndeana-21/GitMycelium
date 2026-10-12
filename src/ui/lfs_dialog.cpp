#include "ui/lfs_dialog.h"

#include "theme/theme.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {
namespace {

/// `*.bin (.gitattributes)` -> `*.bin`: the row is shown with its source, but the
/// pattern is what `git lfs untrack` takes.
QString patternOf(const QString& row) {
    const int open = row.lastIndexOf(QStringLiteral(" ("));
    if (open > 0 && row.endsWith(QLatin1Char(')'))) {
        return row.left(open);
    }
    return row;
}

} // namespace

LfsDialog::LfsDialog(bool available, const QString& version, bool configured,
                     const QStringList& patterns, const QStringList& files, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Git LFS"));
    setMinimumSize(560, 520);
    setStyleSheet(theme::styleSheet());

    auto* root = new QVBoxLayout(this);
    auto* status = new QLabel(
        available ? (configured
                         ? QStringLiteral("%1 — filters configured for this repository.")
                               .arg(version)
                         : QStringLiteral("%1 — filters are not configured here yet (run "
                                          "Initialize).")
                               .arg(version))
                  : QStringLiteral("git-lfs is not installed. Install it (for example "
                                   "`pacman -S git-lfs`, `brew install git-lfs`) and reopen "
                                   "this dialog."),
        this);
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim().name()));
    root->addWidget(status);

    auto* patternsLabel = new QLabel(QStringLiteral("Tracked patterns"), this);
    root->addWidget(patternsLabel);
    m_patternList = new QListWidget(this);
    m_patternList->addItems(patterns);
    m_patternList->setMaximumHeight(theme::scaledPixels(120));
    root->addWidget(m_patternList);

    auto* trackRow = new QHBoxLayout;
    m_patternField = new QLineEdit(this);
    m_patternField->setPlaceholderText(QStringLiteral("*.psd"));
    trackRow->addWidget(m_patternField, 1);
    auto* trackButton = new QPushButton(QStringLiteral("Track"), this);
    auto* untrackButton = new QPushButton(QStringLiteral("Untrack selected"), this);
    trackRow->addWidget(trackButton);
    trackRow->addWidget(untrackButton);
    root->addLayout(trackRow);

    auto* filesLabel = new QLabel(QStringLiteral("Files under LFS"), this);
    root->addWidget(filesLabel);
    auto* fileList = new QListWidget(this);
    fileList->addItems(files);
    fileList->setSelectionMode(QAbstractItemView::NoSelection);
    root->addWidget(fileList, 1);

    auto* buttons = new QHBoxLayout;
    auto* install = new QPushButton(QStringLiteral("Initialize"), this);
    auto* checkout = new QPushButton(QStringLiteral("Checkout"), this);
    auto* fetch = new QPushButton(QStringLiteral("Fetch"), this);
    auto* pull = new QPushButton(QStringLiteral("Pull"), this);
    auto* push = new QPushButton(QStringLiteral("Push"), this);
    auto* prune = new QPushButton(QStringLiteral("Prune"), this);
    buttons->addWidget(install);
    buttons->addWidget(checkout);
    buttons->addStretch(1);
    buttons->addWidget(fetch);
    buttons->addWidget(pull);
    buttons->addWidget(push);
    buttons->addWidget(prune);
    root->addLayout(buttons);

    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(close);

    // Nothing acts without git-lfs, so the mutating controls are greyed rather than
    // answering "not a git command" from four buttons.
    const QList<QPushButton*> mutating{trackButton, untrackButton, install, checkout, fetch,
                                       pull,      push,         prune};
    for (QPushButton* button : mutating) {
        button->setEnabled(available);
    }

    connect(trackButton, &QPushButton::clicked, this, [this] {
        m_action = Action::Track;
        accept();
    });
    connect(untrackButton, &QPushButton::clicked, this, [this] {
        if (!m_patternList->currentItem()) {
            return;
        }
        m_action = Action::Untrack;
        accept();
    });
    connect(install, &QPushButton::clicked, this, [this] {
        m_action = Action::Install;
        accept();
    });
    connect(checkout, &QPushButton::clicked, this, [this] {
        m_action = Action::Checkout;
        accept();
    });
    connect(fetch, &QPushButton::clicked, this, [this] {
        m_action = Action::Fetch;
        accept();
    });
    connect(pull, &QPushButton::clicked, this, [this] {
        m_action = Action::Pull;
        accept();
    });
    connect(push, &QPushButton::clicked, this, [this] {
        m_action = Action::Push;
        accept();
    });
    connect(prune, &QPushButton::clicked, this, [this] {
        m_action = Action::Prune;
        accept();
    });
}

QString LfsDialog::pattern() const {
    if (m_action == Action::Untrack) {
        const QListWidgetItem* item = m_patternList->currentItem();
        return item ? patternOf(item->text()) : QString();
    }
    return m_patternField->text().trimmed();
}

} // namespace gk
