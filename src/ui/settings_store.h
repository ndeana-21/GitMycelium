#pragma once

#include <QString>
#include <QStringList>

namespace gk {

/// A typed view over `QSettings` for the window's preferences.
///
/// Two namespaces exist on purpose. Application settings — theme, zoom, keymap,
/// graph motion — are global and live under `ui/<key>`. Repository settings are
/// keyed by the common git directory, `ui/<group>/<dir>`, so a repository and
/// its linked worktrees share one set. With no repository open the repository
/// namespace collapses to `ui/<group>`, which is exactly what the window already
/// wrote. This is a facade over those keys, not a migration: an existing profile
/// keeps reading.
class SettingsStore {
public:
    explicit SettingsStore(const QString& repositoryPath = QString());

    /// The common git directory repository settings are keyed by, or empty.
    const QString& repositoryPath() const { return m_repositoryPath; }

    /// Full key for a repository group: ("preflight") -> "ui/preflight/<dir>",
    /// or "ui/preflight" with no repository.
    QString repositoryKey(const QString& group) const;
    /// Full key for a global setting: ("theme") -> "ui/theme".
    static QString globalKey(const QString& key);

    bool contains(const QString& key) const;
    bool boolValue(const QString& key, bool fallback = false) const;
    int intValue(const QString& key, int fallback = 0) const;
    QString stringValue(const QString& key, const QString& fallback = QString()) const;
    QStringList listValue(const QString& key, const QStringList& fallback = QStringList()) const;

    void setBool(const QString& key, bool value);
    void setInt(const QString& key, int value);
    void setString(const QString& key, const QString& value);
    void setList(const QString& key, const QStringList& value);
    void remove(const QString& key);
    void sync();

private:
    QString m_repositoryPath;
};

} // namespace gk
