#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QLineEdit;
class QTreeWidget;

namespace gk {

/// One thing the window can do, in the form a palette can list and run it.
///
/// It is data, not a QAction: the same entry feeds the palette, a future
/// configurable keymap (K2) and the headless command report. A QAction built
/// inside a function cannot be discovered or re-bound, which is what kept the
/// window's commands out of reach until now.
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

    /// Commands matching `query`, best matches first. An empty query returns
    /// everything, sorted by title.
    std::vector<const Command*> search(const QString& query) const;

private:
    std::vector<Command> m_commands;
};

/// The palette itself: type to filter, arrows to choose, Enter to run.
///
/// Selecting does not run anything here. The dialog only records the chosen id
/// and accepts; the window runs the command after `exec()` returns, so a command
/// that opens its own dialog does not open it underneath a modal palette.
class CommandPalette : public QDialog {
    Q_OBJECT

public:
    explicit CommandPalette(const CommandRegistry& registry, QWidget* parent = nullptr);

    /// The id chosen, or empty when the palette was dismissed.
    QString chosenId() const { return m_chosenId; }

    /// The ids the palette would show for `query`, in order — for headless checks
    /// that must not open a modal window.
    QStringList matchesFor(const QString& query) const;

    /// Types `query` and runs the first match as Enter would, returning its id.
    /// Nothing is run; the palette merely records the choice, exactly as a click
    /// does.
    QString chooseFirst(const QString& query);

private:
    void refill();
    void runCurrent();
    const Command* currentCommand() const;

    const CommandRegistry& m_registry;
    QLineEdit* m_filter = nullptr;
    QTreeWidget* m_list = nullptr;
    std::vector<const Command*> m_shown;
    QString m_chosenId;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace gk
