#include "ui/profiles.h"

#include "ui/settings_store.h"

#include <QSettings>

namespace gk {
namespace profiles {
namespace {

const QString kListKey = QStringLiteral("ui/profiles/list");

QString definitionPrefix() {
    return QStringLiteral("ui/profiles/def/");
}

QString fieldKey(const QString& name, const QString& field) {
    return definitionPrefix() + name + QLatin1Char('/') + field;
}

} // namespace

QStringList names() {
    QSettings settings;
    return settings.value(kListKey).toStringList();
}

GitProfile load(const QString& name) {
    QSettings settings;
    GitProfile profile;
    profile.name = name;
    profile.userName = settings.value(fieldKey(name, QStringLiteral("user"))).toString();
    profile.email = settings.value(fieldKey(name, QStringLiteral("email"))).toString();
    profile.signingKey = settings.value(fieldKey(name, QStringLiteral("key"))).toString();
    profile.sign = settings.value(fieldKey(name, QStringLiteral("sign")), false).toBool();
    return profile;
}

void save(const GitProfile& profile) {
    if (profile.name.isEmpty()) {
        return;
    }
    QSettings settings;
    // The list is written only when the name is new, so saving a profile twice does
    // not duplicate it or move it in the order.
    QStringList known = names();
    if (!known.contains(profile.name)) {
        known << profile.name;
        settings.setValue(kListKey, known);
    }
    settings.setValue(fieldKey(profile.name, QStringLiteral("user")), profile.userName);
    settings.setValue(fieldKey(profile.name, QStringLiteral("email")), profile.email);
    settings.setValue(fieldKey(profile.name, QStringLiteral("key")), profile.signingKey);
    settings.setValue(fieldKey(profile.name, QStringLiteral("sign")), profile.sign);
}

void remove(const QString& name) {
    if (name.isEmpty()) {
        return;
    }
    QSettings settings;
    QStringList known = names();
    known.removeAll(name);
    settings.setValue(kListKey, known);
    settings.remove(definitionPrefix() + name);
}

QString activeFor(const QString& repositoryPath) {
    SettingsStore store(repositoryPath);
    return store.stringValue(store.repositoryKey(QStringLiteral("profile")));
}

void setActiveFor(const QString& repositoryPath, const QString& name) {
    SettingsStore store(repositoryPath);
    store.setString(store.repositoryKey(QStringLiteral("profile")), name);
}

} // namespace profiles
} // namespace gk
