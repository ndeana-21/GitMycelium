#pragma once

#include "git/hooks.h"

#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>

#include <vector>

class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace gk {

/// The repository's hooks, as a list of switches.
///
/// A hook is a file git runs; the switch is the executable bit. This dialog only
/// reports what the reader chose — the window owns the session and does the writing,
/// the way every other panel works. Enabling a name that has only git's `.sample`
/// is how a sample is adopted: the window copies it into place and marks it
/// executable.
class HooksDialog : public QDialog {
    Q_OBJECT

public:
    HooksDialog(const std::vector<HookEntry>& hooks, const QHash<QString, QString>& contents,
                const QString& hooksPath, QWidget* parent = nullptr);

    /// Hook names the reader left checked. Server-side hooks are never in here:
    /// they run on the remote, not in a local repository.
    QStringList enabledNames() const;

    /// Hook names whose text the reader saved, with the new content. Empty when
    /// nobody edited a hook.
    QHash<QString, QString> editedContents() const { return m_edited; }

    QStringList knownNames() const;

    /// Test seam: check or uncheck a hook without a mouse. Does nothing for a name
    /// that is not in the list.
    void setHookEnabled(const QString& name, bool enabled);
    bool hookEnabled(const QString& name) const;

signals:
    void openHooksFolderRequested();

private:
    QTreeWidgetItem* itemFor(const QString& name) const;
    void showSelectedContent();

    QTreeWidget* m_tree = nullptr;
    QPlainTextEdit* m_editor = nullptr;
    QPushButton* m_saveButton = nullptr;
    QPushButton* m_editButton = nullptr;
    QHash<QString, QString> m_contents; ///< as found, for the "is it changed" question
    QHash<QString, QString> m_edited;
    QString m_hooksPath;
};

} // namespace gk
