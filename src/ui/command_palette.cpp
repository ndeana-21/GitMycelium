#include "ui/command_palette.h"

#include "repo/repo_session.h"
#include "theme/icons.h"
#include "theme/theme.h"
#include "ui/refs_delegate.h"
#include "ui/themed_icons.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace gk {
namespace {

constexpr int kTypeRole = Qt::UserRole + 1;
constexpr int kTargetRole = Qt::UserRole + 2;
constexpr int kToolTipRole = Qt::UserRole + 3;
constexpr int kPillRole = Qt::UserRole + 4;
constexpr int kPillColorRole = Qt::UserRole + 5;
constexpr int kPillKindRole = Qt::UserRole + 6;
/// The chip column has one width, so the subjects after it start in one line.
constexpr int kPillColumn = 170;

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

QString shortRefName(const std::string& source) {
    QString name = QString::fromStdString(source);
    for (const QLatin1String prefix : {QLatin1String("refs/heads/"), QLatin1String("refs/remotes/"),
                                       QLatin1String("refs/tags/"), QLatin1String("refs/")}) {
        if (name.startsWith(prefix)) {
            return name.mid(prefix.size());
        }
    }
    return name;
}

RefKind kindOfSource(const std::string& source) {
    const QString name = QString::fromStdString(source);
    if (name.startsWith(QLatin1String("refs/heads/"))) {
        return RefKind::LocalBranch;
    }
    if (name.startsWith(QLatin1String("refs/remotes/"))) {
        return RefKind::RemoteBranch;
    }
    if (name.startsWith(QLatin1String("refs/tags/"))) {
        return RefKind::Tag;
    }
    return RefKind::Other;
}

QString kindWord(RefKind kind) {
    switch (kind) {
    case RefKind::LocalBranch:
        return QStringLiteral("local");
    case RefKind::RemoteBranch:
        return QStringLiteral("remote");
    case RefKind::Tag:
        return QStringLiteral("tag");
    default:
        return QString();
    }
}

int kindOrder(RefKind kind) {
    switch (kind) {
    case RefKind::LocalBranch:
        return 0;
    case RefKind::RemoteBranch:
        return 1;
    case RefKind::Tag:
        return 2;
    default:
        return 3;
    }
}

QString commitToolTip(const CommitInfo& commit) {
    QString text = QStringLiteral("<b>%1</b>")
                       .arg(QString::fromStdString(commit.summary).toHtmlEscaped());
    if (!commit.body.empty()) {
        text += QStringLiteral("<div style=\"white-space:pre-wrap\">%1</div>")
                    .arg(QString::fromStdString(commit.body).toHtmlEscaped());
    }
    const QDateTime when = QDateTime::fromSecsSinceEpoch(commit.author.whenUnix);
    text += QStringLiteral("<p>%1 &lt;%2&gt;<br>%3 · %4</p>")
                .arg(QString::fromStdString(commit.author.name).toHtmlEscaped(),
                     QString::fromStdString(commit.author.email).toHtmlEscaped(),
                     when.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                     QString::fromStdString(commit.id).left(10));
    return text;
}

/// Block headings span the row on a surface of their own; branch and commit rows lead
/// with the same chip the graph's label column draws.
class PaletteDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const int type = index.siblingAtColumn(0).data(kTypeRole).toInt();
        if (type == 0) {
            paintHeader(painter, option, index);
            return;
        }
        const QString pill = index.data(kPillRole).toString();
        if (index.column() != 0 || pill.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

        const QFontMetrics labelMetrics(theme::labelFont());
        // A branch row is the chip alone and gets the whole cell; a commit's chip stays
        // in its column. The chip adds 8px of padding on each side of its text.
        const bool alone = opt.text.isEmpty();
        const int chipText = (alone ? opt.rect.width() - 8 : kPillColumn) - 16;
        const QString elided = labelMetrics.elidedText(pill, Qt::ElideMiddle, chipText);
        const QPixmap chip = RefsDelegate::dragPixmap(
            elided, index.data(kPillColorRole).toInt(),
            static_cast<RefKind>(index.data(kPillKindRole).toInt()));
        const QSizeF chipSize = chip.deviceIndependentSize();
        const QRect area = opt.rect.adjusted(4, 0, 0, 0);
        const QPoint chipAt(area.left(),
                            area.top() + (area.height() - static_cast<int>(chipSize.height())) / 2);
        painter->drawPixmap(chipAt, chip);

        QRect textRect = area.adjusted(kPillColumn + 8, 0, -4, 0);
        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(QPalette::Text));
        const QString text = opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, textRect.width());
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
    }

private:
    static void paintHeader(QPainter* painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) {
        if (index.column() != 0) {
            return;
        }
        painter->save();
        const QRect rect = option.rect;
        painter->fillRect(rect, theme::surfaceAlt());
        painter->setPen(theme::border());
        painter->drawLine(rect.topLeft(), rect.topRight());
        QFont font = option.font;
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(theme::textDim());
        painter->drawText(rect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                          index.data(Qt::DisplayRole).toString().toUpper());
        painter->restore();
    }
};

} // namespace

bool paletteMatches(const QString& candidate, const QString& query) {
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        return true;
    }
    if (trimmed.contains(QLatin1Char('*')) || trimmed.contains(QLatin1Char('?'))) {
        const QRegularExpression pattern(
            QRegularExpression::wildcardToRegularExpression(
                trimmed, QRegularExpression::UnanchoredWildcardConversion),
            QRegularExpression::CaseInsensitiveOption);
        return pattern.match(candidate).hasMatch();
    }
    return candidate.contains(trimmed, Qt::CaseInsensitive);
}

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
    setWindowTitle(QStringLiteral("Search"));
    setModal(true);
    setMinimumSize(760, 480);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    m_filter->installEventFilter(this);
    QAction* filterIcon =
        m_filter->addAction(icon(Icon::Search, theme::textDim()), QLineEdit::LeadingPosition);
    setThemedIcon(filterIcon, Icon::Search, IconTint::Dim);
    top->addWidget(m_filter, 1);

    auto* modes = new QButtonGroup(this);
    const auto modeButton = [this, modes, top](const QString& text, const QString& tip, Mode mode) {
        auto* button = new QToolButton(this);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        modes->addButton(button);
        top->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, mode] { setMode(mode); });
        return button;
    };
    m_textMode = modeButton(QStringLiteral("Text"),
                            QStringLiteral("Commands, branches, and commits by message or hash"),
                            Mode::Text);
    m_authorMode = modeButton(QStringLiteral("Author"),
                              QStringLiteral("Commits by author name or email"), Mode::Author);
    m_datesMode = modeButton(QStringLiteral("Dates"),
                             QStringLiteral("Commits in a date range, optionally by message"),
                             Mode::Dates);
    m_textMode->setChecked(true);
    layout->addLayout(top);

    m_dateRow = new QWidget(this);
    auto* dates = new QHBoxLayout(m_dateRow);
    dates->setContentsMargins(0, 0, 0, 0);
    const QDate today = QDate::currentDate();
    m_from = new QDateEdit(today.addMonths(-1), m_dateRow);
    m_to = new QDateEdit(today, m_dateRow);
    for (QDateEdit* edit : {m_from, m_to}) {
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        connect(edit, &QDateEdit::dateChanged, this, [this] { scheduleSearch(); });
    }
    dates->addWidget(new QLabel(QStringLiteral("From"), m_dateRow));
    dates->addWidget(m_from);
    dates->addWidget(new QLabel(QStringLiteral("to"), m_dateRow));
    dates->addWidget(m_to);
    dates->addStretch(1);
    layout->addWidget(m_dateRow);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(3);
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(false);
    m_list->setIndentation(0);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setItemDelegate(new PaletteDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->viewport()->installEventFilter(this);
    // The filter keeps the keyboard: arrows are handled for the list from there,
    // so the reader never has to move focus out of the field they are typing in.
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_list->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_list->header()->resizeSection(1, 170);
    m_list->header()->resizeSection(2, 120);
    layout->addWidget(m_list, 1);

    m_searchDelay.setSingleShot(true);
    m_searchDelay.setInterval(150);
    connect(&m_searchDelay, &QTimer::timeout, this, [this] { startSearch(); });

    connect(m_filter, &QLineEdit::textChanged, this, [this] {
        m_readerMoved = false;
        refill();
    });
    connect(m_filter, &QLineEdit::returnPressed, this, [this] { chooseCurrent(); });
    connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        if (rowType(item) != HeaderRow) {
            m_list->setCurrentItem(item);
            chooseCurrent();
        }
    });

    updateModeControls();
    refill();
    m_filter->setFocus();
}

void CommandPalette::setRefs(std::vector<PaletteRef> refs) {
    m_refs = std::move(refs);
    refill();
}

void CommandPalette::setSession(RepoSession* session, bool firstParent) {
    if (m_session) {
        disconnect(m_session, nullptr, this, nullptr);
    }
    m_session = session;
    m_firstParent = firstParent;
    if (m_session) {
        connect(m_session, &RepoSession::commitSearchProgress, this,
                [this](const CommitSearchBatch& batch) { onSearchProgress(batch); });
    }
    refill();
}

void CommandPalette::setPreferHistory(bool prefer) {
    m_preferHistory = prefer;
    m_filter->setPlaceholderText(prefer ? QStringLiteral("Find a branch or commit…")
                                        : m_filter->placeholderText());
    selectFirst();
}

void CommandPalette::setMode(Mode mode) {
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    updateModeControls();
    m_readerMoved = false;
    refill();
}

void CommandPalette::setQuery(const QString& query) {
    m_filter->setText(query);
}

void CommandPalette::setDateRange(const QDate& from, const QDate& to) {
    const QSignalBlocker blockFrom(m_from);
    const QSignalBlocker blockTo(m_to);
    m_from->setDate(from);
    m_to->setDate(to);
    scheduleSearch();
}

void CommandPalette::updateModeControls() {
    m_textMode->setChecked(m_mode == Mode::Text);
    m_authorMode->setChecked(m_mode == Mode::Author);
    m_datesMode->setChecked(m_mode == Mode::Dates);
    m_dateRow->setVisible(m_mode == Mode::Dates);
    switch (m_mode) {
    case Mode::Text:
        m_filter->setPlaceholderText(m_preferHistory
                                         ? QStringLiteral("Find a branch or commit…")
                                         : QStringLiteral("Type a command, branch, or commit…"));
        break;
    case Mode::Author:
        m_filter->setPlaceholderText(QStringLiteral("Author name or email…"));
        break;
    case Mode::Dates:
        m_filter->setPlaceholderText(QStringLiteral("Message (optional)…"));
        break;
    }
}

int CommandPalette::rowType(const QTreeWidgetItem* item) const {
    return item ? item->data(0, kTypeRole).toInt() : HeaderRow;
}

QTreeWidgetItem* CommandPalette::addHeader(const QString& title) {
    auto* item = new QTreeWidgetItem(m_list);
    item->setText(0, title);
    item->setData(0, kTypeRole, HeaderRow);
    item->setFlags(Qt::ItemIsEnabled);
    item->setFirstColumnSpanned(true);
    return item;
}

void CommandPalette::refill() {
    m_list->clear();
    m_commitHeader = nullptr;
    const QString query = m_filter->text();

    if (m_mode == Mode::Text) {
        const std::vector<const Command*> commands = m_registry.search(query);
        if (!commands.empty()) {
            addHeader(QStringLiteral("Commands"));
        }
        for (const Command* command : commands) {
            auto* item = new QTreeWidgetItem(m_list);
            item->setText(0, command->title);
            if (!command->shortcut.isEmpty()) {
                item->setText(2, command->shortcut.toString(QKeySequence::NativeText));
            }
            item->setData(0, Qt::UserRole, command->id);
            item->setData(0, kTypeRole, CommandRow);
            item->setForeground(2, theme::textDim());
            if (!command->isEnabled()) {
                item->setForeground(0, theme::textDim());
            }
        }

        if (!query.trimmed().isEmpty()) {
            std::vector<const PaletteRef*> refs;
            for (const PaletteRef& ref : m_refs) {
                if (paletteMatches(ref.name, query)) {
                    refs.push_back(&ref);
                }
            }
            const QString lowered = query.trimmed().toLower();
            std::stable_sort(refs.begin(), refs.end(), [&lowered](const auto* a, const auto* b) {
                const bool aStarts = a->name.toLower().startsWith(lowered);
                const bool bStarts = b->name.toLower().startsWith(lowered);
                if (aStarts != bStarts) {
                    return aStarts;
                }
                if (kindOrder(a->kind) != kindOrder(b->kind)) {
                    return kindOrder(a->kind) < kindOrder(b->kind);
                }
                return a->name < b->name;
            });
            if (!refs.empty()) {
                addHeader(refs.size() > static_cast<std::size_t>(kRefLimit)
                              ? QStringLiteral("Branches · first %1 of %2")
                                    .arg(kRefLimit)
                                    .arg(refs.size())
                              : QStringLiteral("Branches · %1").arg(refs.size()));
            }
            int shown = 0;
            for (const PaletteRef* ref : refs) {
                if (shown++ == kRefLimit) {
                    break;
                }
                auto* item = new QTreeWidgetItem(m_list);
                item->setText(0, QString());
                item->setText(2, kindWord(ref->kind));
                item->setForeground(2, theme::textDim());
                item->setData(0, Qt::UserRole, ref->name);
                item->setData(0, kTypeRole, RefRow);
                item->setData(0, kTargetRole, ref->target);
                item->setData(0, kPillRole, ref->name);
                item->setData(0, kPillColorRole, ref->colorIndex);
                item->setData(0, kPillKindRole, static_cast<int>(ref->kind));
            }
        }
    }

    scheduleSearch();
    selectFirst();
}

CommitQuery CommandPalette::currentQuery() const {
    CommitQuery query;
    const std::string text = m_filter->text().trimmed().toStdString();
    switch (m_mode) {
    case Mode::Text:
        query.text = text;
        break;
    case Mode::Author:
        query.author = text;
        break;
    case Mode::Dates:
        query.text = text;
        query.since = m_from->date().startOfDay().toSecsSinceEpoch();
        query.until = m_to->date().endOfDay().toSecsSinceEpoch();
        break;
    }
    query.firstParent = m_firstParent;
    return query;
}

void CommandPalette::scheduleSearch() {
    m_searchDelay.stop();
    if (m_session) {
        m_session->stopCommitSearch();
    }
    m_search = 0;
    m_commitCount = 0;
    m_searchError.clear();
    m_searching = false;
    if (m_commitHeader) {
        while (m_list->topLevelItemCount() > 0 &&
               m_list->topLevelItem(m_list->topLevelItemCount() - 1) != m_commitHeader) {
            delete m_list->takeTopLevelItem(m_list->topLevelItemCount() - 1);
        }
        delete m_list->takeTopLevelItem(m_list->indexOfTopLevelItem(m_commitHeader));
        m_commitHeader = nullptr;
    }
    const CommitQuery query = currentQuery();
    if (!m_session || (query.text.empty() && query.author.empty() && query.since == 0)) {
        return;
    }
    m_searching = true;
    m_commitHeader = addHeader(QString());
    updateCommitHeader();
    m_searchDelay.start();
}

void CommandPalette::startSearch() {
    if (!m_session || !m_searching) {
        return;
    }
    m_search = m_session->startCommitSearch(currentQuery(), kCommitLimit);
}

void CommandPalette::onSearchProgress(const CommitSearchBatch& batch) {
    if (batch.search != m_search || m_search == 0) {
        return;
    }
    for (const CommitHit& hit : batch.hits) {
        addCommitRow(hit);
    }
    if (batch.done) {
        m_searching = false;
        m_searchError = batch.error;
    }
    updateCommitHeader();
    if (!m_readerMoved) {
        selectFirst();
    }
}

void CommandPalette::addCommitRow(const CommitHit& hit) {
    const CommitInfo& commit = hit.commit;
    const QString pill = hit.source.empty() ? QStringLiteral("HEAD") : shortRefName(hit.source);
    int colorIndex = 0;
    RefKind kind = kindOfSource(hit.source);
    for (const PaletteRef& ref : m_refs) {
        if (ref.name == pill) {
            colorIndex = ref.colorIndex;
            kind = ref.kind;
            break;
        }
    }
    auto* item = new QTreeWidgetItem(m_list);
    item->setText(0, QString::fromStdString(commit.summary));
    item->setText(1, QString::fromStdString(commit.author.name));
    item->setText(2, QDateTime::fromSecsSinceEpoch(commit.author.whenUnix)
                         .toString(QStringLiteral("yyyy-MM-dd")));
    item->setForeground(1, theme::textDim());
    item->setForeground(2, theme::textDim());
    item->setData(0, Qt::UserRole, QString::fromStdString(commit.id));
    item->setData(0, kTypeRole, CommitRow);
    item->setData(0, kToolTipRole, commitToolTip(commit));
    item->setData(0, kPillRole, pill);
    item->setData(0, kPillColorRole, colorIndex);
    item->setData(0, kPillKindRole, static_cast<int>(kind));
    ++m_commitCount;
}

void CommandPalette::updateCommitHeader() {
    if (!m_commitHeader) {
        return;
    }
    QString status;
    if (!m_searchError.isEmpty()) {
        status = QStringLiteral("git: %1").arg(m_searchError);
    } else if (m_searching) {
        status = m_commitCount > 0 ? QStringLiteral("%1, searching…").arg(m_commitCount)
                                   : QStringLiteral("searching…");
    } else if (m_commitCount == 0) {
        status = QStringLiteral("nothing found");
    } else if (m_commitCount >= kCommitLimit) {
        status = QStringLiteral("first %1").arg(m_commitCount);
    } else {
        status = QString::number(m_commitCount);
    }
    m_commitHeader->setText(0, QStringLiteral("Commits · %1").arg(status));
}

void CommandPalette::selectFirst() {
    QTreeWidgetItem* first = nullptr;
    QTreeWidgetItem* firstHistory = nullptr;
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        QTreeWidgetItem* item = m_list->topLevelItem(row);
        const int type = rowType(item);
        if (type == HeaderRow) {
            continue;
        }
        if (!first) {
            first = item;
        }
        if (type != CommandRow) {
            firstHistory = item;
            break;
        }
    }
    QTreeWidgetItem* chosen = m_preferHistory && firstHistory ? firstHistory : first;
    if (chosen && chosen != m_list->currentItem()) {
        m_list->setCurrentItem(chosen);
    }
}

void CommandPalette::moveCurrent(int step) {
    const int count = m_list->topLevelItemCount();
    QTreeWidgetItem* current = m_list->currentItem();
    int row = current ? m_list->indexOfTopLevelItem(current) : -1;
    const int direction = step > 0 ? 1 : -1;
    int remaining = std::abs(step);
    for (int candidate = row + direction; candidate >= 0 && candidate < count && remaining > 0;
         candidate += direction) {
        if (rowType(m_list->topLevelItem(candidate)) != HeaderRow) {
            row = candidate;
            --remaining;
        }
    }
    if (row >= 0) {
        m_list->setCurrentItem(m_list->topLevelItem(row));
        m_readerMoved = true;
    }
}

void CommandPalette::chooseCurrent() {
    QTreeWidgetItem* item = m_list->currentItem();
    const int type = rowType(item);
    const QString id = item ? item->data(0, Qt::UserRole).toString() : QString();
    switch (type) {
    case CommandRow: {
        const std::vector<Command>& commands = m_registry.commands();
        const auto found = std::find_if(commands.begin(), commands.end(),
                                        [&id](const Command& command) { return command.id == id; });
        if (found == commands.end() || !found->isEnabled()) {
            return;
        }
        m_choice = Choice::Command;
        break;
    }
    case RefRow:
        m_choice = Choice::Ref;
        m_chosenTarget = item->data(0, kTargetRole).toString();
        break;
    case CommitRow:
        m_choice = Choice::Commit;
        break;
    default:
        return;
    }
    m_chosenId = id;
    if (m_session) {
        m_session->stopCommitSearch();
    }
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
    selectFirst();
    chooseCurrent();
    return m_chosenId;
}

QStringList CommandPalette::rowsForDiagnostics(int timeoutMs) {
    QElapsedTimer waited;
    waited.start();
    while ((m_searching || m_searchDelay.isActive()) && waited.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QStringList rows;
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        const QTreeWidgetItem* item = m_list->topLevelItem(row);
        const QString id = item->data(0, Qt::UserRole).toString();
        const QString current = item == m_list->currentItem() ? QStringLiteral("> ") : QString();
        switch (rowType(item)) {
        case HeaderRow:
            rows << QStringLiteral("== %1").arg(item->text(0));
            break;
        case CommandRow:
            rows << current + QStringLiteral("command %1").arg(id);
            break;
        case RefRow:
            rows << current + QStringLiteral("ref %1").arg(id);
            break;
        case CommitRow:
            rows << current + QStringLiteral("commit %1 [%2] %3 | %4 | %5")
                                  .arg(id.left(8), item->data(0, kPillRole).toString(),
                                       item->text(0), item->text(1), item->text(2));
            break;
        }
    }
    return rows;
}

QString CommandPalette::currentRowText() const {
    const QTreeWidgetItem* item = m_list->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

QString CommandPalette::toolTipForRow(int row) const {
    const QTreeWidgetItem* item = m_list->topLevelItem(row);
    return item ? item->data(0, kToolTipRole).toString() : QString();
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_filter && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Down:
            moveCurrent(1);
            return true;
        case Qt::Key_Up:
            moveCurrent(-1);
            return true;
        case Qt::Key_PageDown:
            moveCurrent(10);
            return true;
        case Qt::Key_PageUp:
            moveCurrent(-10);
            return true;
        default:
            break;
        }
    }
    if (m_list && watched == m_list->viewport()) {
        // A commit's whole message shows at once under the mouse: the subject is cut to
        // the column, and the reader is scanning, not waiting for a tooltip delay.
        if (event->type() == QEvent::MouseMove) {
            const QPoint pos = static_cast<QMouseEvent*>(event)->position().toPoint();
            QTreeWidgetItem* item = m_list->itemAt(pos);
            const QString tip = item ? item->data(0, kToolTipRole).toString() : QString();
            if (tip.isEmpty()) {
                QToolTip::hideText();
            } else {
                QToolTip::showText(m_list->viewport()->mapToGlobal(pos), tip, m_list->viewport(),
                                   m_list->visualItemRect(item));
            }
        } else if (event->type() == QEvent::ToolTip || event->type() == QEvent::Leave) {
            if (event->type() == QEvent::Leave) {
                QToolTip::hideText();
            }
            return event->type() == QEvent::ToolTip;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace gk
