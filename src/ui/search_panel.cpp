#include "ui/search_panel.h"

#include "theme/icons.h"
#include "theme/theme.h"
#include "ui/themed_icons.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace gk {

namespace {
constexpr int kResultLimit = 200;

CommitSearchField fieldFromIndex(int index) {
    switch (index) {
    case 1:
        return CommitSearchField::Author;
    case 2:
        return CommitSearchField::Sha;
    default:
        return CommitSearchField::Message;
    }
}

int indexForField(CommitSearchField field) {
    switch (field) {
    case CommitSearchField::Author:
        return 1;
    case CommitSearchField::Sha:
        return 2;
    case CommitSearchField::Message:
        return 0;
    }
    return 0;
}
} // namespace

void SearchPanel::setSession(RepoSession* session) {
    m_session = session;
}

SearchPanel::SearchPanel(RepoSession* session, QWidget* parent)
    : QWidget(parent), m_session(session) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* row = new QHBoxLayout();
    m_query = new QLineEdit(this);
    m_query->setPlaceholderText(QStringLiteral("Search commits…"));
    m_query->setClearButtonEnabled(true);
    QAction* queryIcon =
        m_query->addAction(icon(Icon::Search, theme::textDim()), QLineEdit::LeadingPosition);
    setThemedIcon(queryIcon, Icon::Search, IconTint::Dim);
    m_field = new QComboBox(this);
    m_field->addItems({QStringLiteral("Message"), QStringLiteral("Author"), QStringLiteral("SHA")});
    row->addWidget(m_query, 1);
    row->addWidget(m_field);
    layout->addLayout(row);

    m_summary = new QLabel(QStringLiteral("Type a query and press Enter."), this);
    // Styled by object name rather than an inline colour: the panel is a dock that
    // outlives a theme change, and a literal here would keep the old palette.
    m_summary->setObjectName(QStringLiteral("searchSummary"));
    layout->addWidget(m_summary);

    m_results = new QTreeWidget(this);
    m_results->setColumnCount(4);
    m_results->setHeaderLabels({QStringLiteral("Commit"), QStringLiteral("Author"),
                                QStringLiteral("Date"), QStringLiteral("Message")});
    m_results->setRootIsDecorated(false);
    m_results->setUniformRowHeights(true);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(m_results, 1);

    connect(m_query, &QLineEdit::returnPressed, this, &SearchPanel::runSearch);
    connect(m_field, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { runSearch(); });
    connect(m_results, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (item) {
            emit commitActivated(item->data(0, Qt::UserRole).toString());
        }
    });
    connect(m_results, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item) {
            emit commitActivated(item->data(0, Qt::UserRole).toString());
        }
    });
    if (m_session) {
        connect(m_session, &RepoSession::commitSearchFinished, this, &SearchPanel::onResults);
    }
}

CommitSearchField SearchPanel::field() const {
    return fieldFromIndex(m_field->currentIndex());
}

void SearchPanel::setField(CommitSearchField field) {
    const int index = indexForField(field);
    if (m_field->currentIndex() != index) {
        m_field->setCurrentIndex(index);
    }
}

void SearchPanel::setQuery(const QString& query) {
    m_query->setText(query);
}

void SearchPanel::focusQuery() {
    m_query->setFocus();
    m_query->selectAll();
}

void SearchPanel::runSearch() {
    const QString query = m_query->text().trimmed();
    if (query.isEmpty()) {
        clearResults();
        m_summary->setText(QStringLiteral("Type a query and press Enter."));
        return;
    }
    if (!m_session) {
        return;
    }
    m_summary->setText(QStringLiteral("Searching…"));
    m_session->requestCommitSearch(query, field(), kResultLimit);
}

void SearchPanel::onResults(gk::CommitSearchResults results) {
    // A result for a query the reader has already typed past is stale; dropping it
    // keeps the list from flickering back to an earlier search.
    if (results.query != m_query->text().trimmed()) {
        return;
    }
    std::vector<CommitInfo> commits;
    commits.reserve(static_cast<std::size_t>(results.commits.size()));
    for (const CommitInfo& commit : results.commits) {
        commits.push_back(commit);
    }
    showResults(results.query, fieldFromIndex(results.field), commits);
}

void SearchPanel::showResults(const QString& query, CommitSearchField field,
                              const std::vector<CommitInfo>& commits) {
    Q_UNUSED(field);
    m_results->clear();
    for (const CommitInfo& commit : commits) {
        const QString oid = QString::fromStdString(commit.id);
        auto* item = new QTreeWidgetItem(m_results);
        item->setText(0, oid.left(8));
        item->setText(1, QString::fromStdString(commit.author.name));
        item->setText(2, QDateTime::fromSecsSinceEpoch(commit.author.whenUnix)
                             .toString(QStringLiteral("yyyy-MM-dd")));
        item->setText(3, QString::fromStdString(commit.summary));
        item->setData(0, Qt::UserRole, oid);
    }
    m_summary->setText(commits.empty()
                           ? QStringLiteral("No commits match \"%1\".").arg(query)
                           : QStringLiteral("%1 result(s) for \"%2\".")
                                 .arg(commits.size())
                                 .arg(query));
}

void SearchPanel::clearResults() {
    m_results->clear();
}

int SearchPanel::resultCount() const {
    return m_results->topLevelItemCount();
}

QString SearchPanel::resultIdAt(int row) const {
    if (row < 0 || row >= m_results->topLevelItemCount()) {
        return {};
    }
    return m_results->topLevelItem(row)->data(0, Qt::UserRole).toString();
}

} // namespace gk
