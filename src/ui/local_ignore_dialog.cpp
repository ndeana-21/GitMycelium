#include "ui/local_ignore_dialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {

LocalIgnoreDialog::LocalIgnoreDialog(const QStringList& patterns, const QStringList& hiddenFiles,
                                     QWidget* parent)
    : QDialog(parent), m_patterns(patterns), m_hidden(hiddenFiles) {
    setWindowTitle(QStringLiteral("Local Ignore Rules"));
    resize(560, 480);
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        QStringLiteral("Hidden in this checkout only. Nothing here is committed or shared: "
                       "the repository's .gitignore is not touched."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    layout->addWidget(new QLabel(QStringLiteral("Patterns in .git/info/exclude"), this));
    m_patternList = new QListWidget(this);
    m_patternList->setObjectName(QStringLiteral("localExcludePatterns"));
    m_patternList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_patternList->addItems(patterns);
    layout->addWidget(m_patternList, 1);

    auto* addRow = new QHBoxLayout;
    m_newPattern = new QLineEdit(this);
    m_newPattern->setPlaceholderText(QStringLiteral("Pattern, e.g. *.log or build/"));
    auto* add = new QPushButton(QStringLiteral("Add"), this);
    auto* remove = new QPushButton(QStringLiteral("Remove Selected"), this);
    addRow->addWidget(m_newPattern, 1);
    addRow->addWidget(add);
    addRow->addWidget(remove);
    layout->addLayout(addRow);
    connect(add, &QPushButton::clicked, this, [this] {
        addPattern(m_newPattern->text());
        m_newPattern->clear();
    });
    connect(m_newPattern, &QLineEdit::returnPressed, add, &QPushButton::click);
    connect(remove, &QPushButton::clicked, this, [this] {
        for (QListWidgetItem* item : m_patternList->selectedItems()) {
            delete item;
        }
    });

    layout->addWidget(
        new QLabel(QStringLiteral("Tracked files whose changes are hidden here (skip-worktree)"),
                   this));
    m_hiddenList = new QListWidget(this);
    m_hiddenList->setObjectName(QStringLiteral("skipWorktreeFiles"));
    m_hiddenList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_hiddenList->addItems(hiddenFiles);
    layout->addWidget(m_hiddenList, 1);
    auto* unhide = new QPushButton(QStringLiteral("Show Changes Again"), this);
    auto* unhideRow = new QHBoxLayout;
    unhideRow->addStretch(1);
    unhideRow->addWidget(unhide);
    layout->addLayout(unhideRow);
    connect(unhide, &QPushButton::clicked, this, [this] {
        for (QListWidgetItem* item : m_hiddenList->selectedItems()) {
            delete item;
        }
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QStringList LocalIgnoreDialog::itemsOf(const QListWidget* list) {
    QStringList out;
    for (int i = 0; i < list->count(); ++i) {
        out << list->item(i)->text();
    }
    return out;
}

void LocalIgnoreDialog::addPattern(const QString& pattern) {
    const QString trimmed = pattern.trimmed();
    if (trimmed.isEmpty() || itemsOf(m_patternList).contains(trimmed)) {
        return;
    }
    m_patternList->addItem(trimmed);
}

void LocalIgnoreDialog::removePattern(const QString& pattern) {
    for (QListWidgetItem* item : m_patternList->findItems(pattern, Qt::MatchExactly)) {
        delete item;
    }
}

void LocalIgnoreDialog::unhideFile(const QString& path) {
    for (QListWidgetItem* item : m_hiddenList->findItems(path, Qt::MatchExactly)) {
        delete item;
    }
}

QStringList LocalIgnoreDialog::addedPatterns() const {
    QStringList out;
    for (const QString& pattern : itemsOf(m_patternList)) {
        if (!m_patterns.contains(pattern)) {
            out << pattern;
        }
    }
    return out;
}

QStringList LocalIgnoreDialog::removedPatterns() const {
    const QStringList now = itemsOf(m_patternList);
    QStringList out;
    for (const QString& pattern : m_patterns) {
        if (!now.contains(pattern)) {
            out << pattern;
        }
    }
    return out;
}

QStringList LocalIgnoreDialog::unhiddenFiles() const {
    const QStringList now = itemsOf(m_hiddenList);
    QStringList out;
    for (const QString& path : m_hidden) {
        if (!now.contains(path)) {
            out << path;
        }
    }
    return out;
}

} // namespace gk
