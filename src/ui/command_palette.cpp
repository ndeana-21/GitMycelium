#include "ui/command_palette.h"

#include "theme/icons.h"
#include "theme/theme.h"
#include "ui/themed_icons.h"

#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace gk {
namespace {

int wordScore(const QString& title, const QString& category, const QString& word) {
    const QString lowerTitle = title.toLower();
    if (lowerTitle.startsWith(word)) {
        return 0;
    }
    const QStringList titleWords =
        lowerTitle.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& titleWord : titleWords) {
        if (titleWord.startsWith(word)) {
            return 1;
        }
    }
    if (lowerTitle.contains(word)) {
        return 2;
    }
    if (category.toLower().contains(word)) {
        return 3;
    }
    return -1;
}

} // namespace

std::vector<const Command*> CommandRegistry::search(const QString& query) const {
    const QStringList words =
        query.trimmed().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);

    std::vector<std::pair<const Command*, int>> scored;
    scored.reserve(m_commands.size());
    for (const Command& command : m_commands) {
        int score = 0;
        bool matched = true;
        for (const QString& word : words) {
            const int wordMatch = wordScore(command.title, command.category, word);
            if (wordMatch < 0) {
                matched = false;
                break;
            }
            score += wordMatch;
        }
        if (matched) {
            scored.emplace_back(&command, score);
        }
    }

    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second < b.second;
        }
        return a.first->title.toLower() < b.first->title.toLower();
    });

    std::vector<const Command*> out;
    out.reserve(scored.size());
    for (const auto& entry : scored) {
        out.push_back(entry.first);
    }
    return out;
}

CommandPalette::CommandPalette(const CommandRegistry& registry, QWidget* parent)
    : QDialog(parent), m_registry(registry) {
    setWindowTitle(QStringLiteral("Command Palette"));
    setModal(true);
    setMinimumSize(560, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("Type a command…"));
    m_filter->setClearButtonEnabled(true);
    m_filter->installEventFilter(this);
    QAction* filterIcon =
        m_filter->addAction(icon(Icon::Search, theme::textDim()), QLineEdit::LeadingPosition);
    setThemedIcon(filterIcon, Icon::Search, IconTint::Dim);
    layout->addWidget(m_filter);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(2);
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    // The filter keeps the keyboard: arrows are handled for the list from there,
    // so the reader never has to move focus out of the field they are typing in.
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_list, 1);

    connect(m_filter, &QLineEdit::textChanged, this, [this] { refill(); });
    connect(m_filter, &QLineEdit::returnPressed, this, [this] { runCurrent(); });
    connect(m_list, &QTreeWidget::itemClicked, this, [this] { runCurrent(); });
    connect(m_list, &QTreeWidget::itemActivated, this, [this] { runCurrent(); });

    refill();
    m_filter->setFocus();
}

void CommandPalette::refill() {
    m_shown = m_registry.search(m_filter->text());
    m_list->clear();
    for (const Command* command : m_shown) {
        auto* item = new QTreeWidgetItem(m_list);
        item->setText(0, command->title);
        if (!command->shortcut.isEmpty()) {
            item->setText(1, command->shortcut.toString(QKeySequence::NativeText));
        }
        item->setData(0, Qt::UserRole, command->id);
        if (!command->isEnabled()) {
            item->setForeground(0, theme::textDim());
            item->setForeground(1, theme::textDim());
        }
    }
    if (m_list->topLevelItemCount() > 0) {
        m_list->setCurrentItem(m_list->topLevelItem(0));
    }
}

const Command* CommandPalette::currentCommand() const {
    QTreeWidgetItem* item = m_list->currentItem();
    if (!item) {
        return nullptr;
    }
    const QString id = item->data(0, Qt::UserRole).toString();
    for (const Command* command : m_shown) {
        if (command->id == id) {
            return command;
        }
    }
    return nullptr;
}

void CommandPalette::runCurrent() {
    const Command* command = currentCommand();
    if (!command || !command->isEnabled()) {
        return;
    }
    m_chosenId = command->id;
    accept();
}

QStringList CommandPalette::matchesFor(const QString& query) const {
    QStringList ids;
    for (const Command* command : m_registry.search(query)) {
        ids << command->id;
    }
    return ids;
}

QString CommandPalette::chooseFirst(const QString& query) {
    m_filter->setText(query);
    refill();
    runCurrent();
    return m_chosenId;
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_filter && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const int count = m_list->topLevelItemCount();
        if (count > 0 && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
            const int current =
                m_list->currentItem() ? m_list->indexOfTopLevelItem(m_list->currentItem()) : -1;
            int row = current + (key->key() == Qt::Key_Down ? 1 : -1);
            row = std::clamp(row, 0, count - 1);
            m_list->setCurrentItem(m_list->topLevelItem(row));
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace gk
