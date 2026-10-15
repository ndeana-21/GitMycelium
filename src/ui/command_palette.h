#pragma once

#include "core/types.h"

#include <QDateEdit>
#include <QDialog>
#include <QKeySequence>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <vector>

class QLineEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace gk {

class RepoSession;
struct CommitSearchBatch;

struct Command {
    QString id;
    QString title;
    QString category;
    QKeySequence shortcut;
    std::function<bool()> enabled; ///< empty means "always enabled"
    std::function<void()> run;

    bool isEnabled() const { return !enabled || enabled(); }
};

/// The window's commands, as data.
///
/// Matching is deliberately simple and predictable: the query is split on
/// spaces and every word must match either the title or the category. A word
/// scores best when the title starts with it, then when a title word starts
/// with it, then when it merely appears; the scores are summed and the list is
/// sorted, ties broken by title, so the same query always gives the same order.
class CommandRegistry {
public:
    void clear() { m_commands.clear(); }
    void add(Command command) { m_commands.push_back(std::move(command)); }
    const std::vector<Command>& commands() const { return m_commands; }

    std::vector<const Command*> search(const QString& query) const;

private:
    std::vector<Command> m_commands;
};

/// A branch or tag the palette can jump to.
struct PaletteRef {
    QString name;
    RefKind kind = RefKind::Other;
    QString target;
    int colorIndex = 0;
};

/// `*text*`: a case-insensitive substring, or a wildcard pattern when the query holds
/// `*` or `?`. The same rule `git log` is given for commits.
bool paletteMatches(const QString& candidate, const QString& query);

/// One window for everything a reader types to find: commands, then branches, then
/// commits, each block under its own heading.
///
/// Commits come from a `git log` running on a worker while the reader types; rows are
/// added as git finds them and each keystroke stops the search before. Besides the
/// text (message or hash), the search can be by author or by a date range.
///
/// Selecting does not run anything here. The dialog only records the choice and
/// accepts; the window acts after `exec()` returns, so a command that opens its own
/// dialog does not open it underneath a modal palette.
class CommandPalette : public QDialog {
    Q_OBJECT

public:
    enum class Mode { Text, Author, Dates };
    enum class Choice { None, Command, Ref, Commit };
    static constexpr int kCommitLimit = 500;
    static constexpr int kRefLimit = 30;

    explicit CommandPalette(const CommandRegistry& registry, QWidget* parent = nullptr);

    void setRefs(std::vector<PaletteRef> refs);
    /// Turns on the commit block. `firstParent` searches only what the folded graph
    /// draws, so every result can be revealed.
    void setSession(RepoSession* session, bool firstParent);
    /// Ctrl+F: the first branch or commit is the current row, not the first command.
    void setPreferHistory(bool prefer);
    void setMode(Mode mode);
    void setQuery(const QString& query);
    void setDateRange(const QDate& from, const QDate& to);

    Choice choice() const { return m_choice; }
    /// The command id, the ref name, or the commit id.
    QString chosenId() const { return m_chosenId; }
    /// The tip a chosen ref points at.
    QString chosenTarget() const { return m_chosenTarget; }

    /// The ids the palette would show for `query`, in order — for headless checks
    /// that must not open a modal window.
    QStringList matchesFor(const QString& query) const;

    /// Types `query` and runs the first match as Enter would, returning its id.
    /// Nothing is run; the palette merely records the choice, exactly as a click
    /// does.
    QString chooseFirst(const QString& query);

    /// Every row as the reader sees it, block headings included; waits for the commit
    /// search to finish first.
    QStringList rowsForDiagnostics(int timeoutMs = 10000);
    bool searching() const { return m_searching; }
    /// Chooses the current row, as Enter does.
    void chooseCurrent();
    /// Moves the current row as the arrows do.
    void moveCurrent(int step);
    QString currentRowText() const;
    QString toolTipForRow(int row) const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum RowType { HeaderRow = 0, CommandRow, RefRow, CommitRow };

    void refill();
    void scheduleSearch();
    void startSearch();
    void onSearchProgress(const CommitSearchBatch& batch);
    void addCommitRow(const CommitHit& hit);
    void updateCommitHeader();
    QTreeWidgetItem* addHeader(const QString& title);
    void selectFirst();
    void updateModeControls();
    CommitQuery currentQuery() const;
    int rowType(const QTreeWidgetItem* item) const;

    const CommandRegistry& m_registry;
    std::vector<PaletteRef> m_refs;
    RepoSession* m_session = nullptr;
    bool m_firstParent = false;
    bool m_preferHistory = false;
    Mode m_mode = Mode::Text;

    QLineEdit* m_filter = nullptr;
    QToolButton* m_textMode = nullptr;
    QToolButton* m_authorMode = nullptr;
    QToolButton* m_datesMode = nullptr;
    QWidget* m_dateRow = nullptr;
    QDateEdit* m_from = nullptr;
    QDateEdit* m_to = nullptr;
    QTreeWidget* m_list = nullptr;
    QTreeWidgetItem* m_commitHeader = nullptr;
    QTimer m_searchDelay;

    quint64 m_search = 0;
    bool m_searching = false;
    int m_commitCount = 0;
    QString m_searchError;
    bool m_readerMoved = false;

    Choice m_choice = Choice::None;
    QString m_chosenId;
    QString m_chosenTarget;
};

} // namespace gk
