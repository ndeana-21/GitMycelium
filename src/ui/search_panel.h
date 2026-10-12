#pragma once

#include "core/types.h"
#include "repo/repo_session.h"

#include <QString>
#include <QWidget>

#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;

namespace gk {

/// The commit search panel: a query, which field it looks at, and the commits that
/// matched, newest first. It holds no repository of its own — the session runs the
/// search on a worker and the panel fills in what comes back — and choosing a
/// result only reports the id, because revealing it is the graph's job.
class SearchPanel : public QWidget {
    Q_OBJECT

public:
    explicit SearchPanel(RepoSession* session, QWidget* parent = nullptr);

    /// Re-points the panel at another repository. The panel is shared across the
    /// window's repository tabs, so a tab switch hands it a different session.
    void setSession(RepoSession* session);

    /// Runs the current query through the session. An empty query clears the list.
    void runSearch();
    void focusQuery();
    void setQuery(const QString& query);
    void setField(CommitSearchField field);
    CommitSearchField field() const;

    int resultCount() const;
    QString resultIdAt(int row) const;
    void clearResults();

    /// Fills the list from a result set the caller already has, without a second
    /// search — the headless path and the tests use this.
    void showResults(const QString& query, CommitSearchField field,
                     const std::vector<CommitInfo>& commits);

signals:
    void commitActivated(QString oid);

private slots:
    void onResults(gk::CommitSearchResults results);

private:
    RepoSession* m_session = nullptr;
    QLineEdit* m_query = nullptr;
    QComboBox* m_field = nullptr;
    QTreeWidget* m_results = nullptr;
    QLabel* m_summary = nullptr;
};

} // namespace gk
