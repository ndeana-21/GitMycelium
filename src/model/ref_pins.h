#pragma once

#include <QString>
#include <QStringList>

namespace gk {

/// References pinned to the top of the branch list, remembered per repository.
///
/// Pins live outside the model on purpose: the model is rebuilt on every fetch
/// and on every keystroke in the filter, while a pin is a decision the user made
/// once. Keyed by the repository's canonical path, so two checkouts of the same
/// project keep their own lists.
class RefPins {
public:
    RefPins();
    /// Uses `storePath` instead — how a test gets its own file.
    explicit RefPins(QString storePath);

    /// Loads the pins recorded for `repoPath`. A missing or unreadable file is
    /// not an error: it means nothing has been pinned yet. `legacyRepoPath`, when
    /// given, is where the pins used to live (the checkout path, before they were
    /// keyed by the common git dir); a non-empty entry there is adopted and moved.
    void load(const QString& repoPath, const QString& legacyRepoPath = QString());

    bool isPinned(const QString& name) const;
    /// Adds or removes a pin. A folder is pinned by its prefix with a trailing
    /// slash, so `feature` the folder and `feature` the branch stay distinct.
    void setPinned(const QString& name, bool pinned);

    /// Oldest first.
    const QStringList& names() const { return m_names; }

    void clear();

    static QString defaultStorePath();

private:
    void save() const;

    QString m_storePath;
    QString m_repoPath;
    QStringList m_names;
};

} // namespace gk
