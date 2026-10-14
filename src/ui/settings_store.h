#pragma once

#include <QString>
#include <QStringList>

namespace gk {

class SettingsStore {
public:
    explicit SettingsStore(const QString& repositoryPath = QString());

    const QString& repositoryPath() const { return m_repositoryPath; }

    /// Full key for a repository group: ("preflight") -> "ui/preflight/<dir>",
    /// or "ui/preflight" with no repository.
    QString repositoryKey(const QString& group) const;
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
