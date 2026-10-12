#include "ui/settings_store.h"

#include <QSettings>

namespace gk {

SettingsStore::SettingsStore(const QString& repositoryPath) : m_repositoryPath(repositoryPath) {}

QString SettingsStore::repositoryKey(const QString& group) const {
    const QString base = QStringLiteral("ui/") + group;
    return m_repositoryPath.isEmpty() ? base : base + QLatin1Char('/') + m_repositoryPath;
}

QString SettingsStore::globalKey(const QString& key) {
    return QStringLiteral("ui/") + key;
}

bool SettingsStore::contains(const QString& key) const {
    QSettings settings;
    return settings.contains(key);
}

bool SettingsStore::boolValue(const QString& key, bool fallback) const {
    QSettings settings;
    return settings.value(key, fallback).toBool();
}

int SettingsStore::intValue(const QString& key, int fallback) const {
    QSettings settings;
    return settings.value(key, fallback).toInt();
}

QString SettingsStore::stringValue(const QString& key, const QString& fallback) const {
    QSettings settings;
    return settings.value(key, fallback).toString();
}

QStringList SettingsStore::listValue(const QString& key, const QStringList& fallback) const {
    QSettings settings;
    return settings.value(key, fallback).toStringList();
}

void SettingsStore::setBool(const QString& key, bool value) {
    QSettings settings;
    settings.setValue(key, value);
}

void SettingsStore::setInt(const QString& key, int value) {
    QSettings settings;
    settings.setValue(key, value);
}

void SettingsStore::setString(const QString& key, const QString& value) {
    QSettings settings;
    settings.setValue(key, value);
}

void SettingsStore::setList(const QString& key, const QStringList& value) {
    QSettings settings;
    settings.setValue(key, value);
}

void SettingsStore::remove(const QString& key) {
    QSettings settings;
    settings.remove(key);
}

void SettingsStore::sync() {
    QSettings settings;
    settings.sync();
}

} // namespace gk
