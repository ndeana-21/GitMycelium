#include "model/ref_visibility.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace gk {
namespace {

QString presetId(VisibilityPreset preset) {
    switch (preset) {
    case VisibilityPreset::Mine: return QStringLiteral("mine");
    case VisibilityPreset::Recent30: return QStringLiteral("recent");
    case VisibilityPreset::Prefix: return QStringLiteral("prefix");
    case VisibilityPreset::NoUpstream: return QStringLiteral("no-upstream");
    case VisibilityPreset::Ahead: return QStringLiteral("ahead");
    case VisibilityPreset::Behind: return QStringLiteral("behind");
    case VisibilityPreset::None: break;
    }
    return QStringLiteral("none");
}

VisibilityPreset presetFromId(const QString& id) {
    if (id == QLatin1String("mine")) return VisibilityPreset::Mine;
    if (id == QLatin1String("recent")) return VisibilityPreset::Recent30;
    if (id == QLatin1String("prefix")) return VisibilityPreset::Prefix;
    if (id == QLatin1String("no-upstream")) return VisibilityPreset::NoUpstream;
    if (id == QLatin1String("ahead")) return VisibilityPreset::Ahead;
    if (id == QLatin1String("behind")) return VisibilityPreset::Behind;
    return VisibilityPreset::None;
}

QJsonObject readTable(const QString& storePath) {
    QFile file(storePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

void writeTable(const QString& storePath, const QJsonObject& root) {
    QDir().mkpath(QFileInfo(storePath).absolutePath());
    // QSaveFile: a crash mid-write must not leave a truncated file behind, since
    // the next launch would then silently lose every repository's visibility.
    QSaveFile out(storePath);
    if (!out.open(QIODevice::WriteOnly)) {
        return;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.commit();
}

} // namespace

QString RefVisibility::defaultStorePath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/visibility.json");
}

RefVisibility::RefVisibility() : m_storePath(defaultStorePath()) {}

RefVisibility::RefVisibility(QString storePath) : m_storePath(std::move(storePath)) {}

void RefVisibility::load(const QString& repoPath, const QString& legacyRepoPath) {
    m_repoPath = repoPath;
    m_hidden.clear();
    m_solo.clear();
    m_smart = false;
    m_preset = VisibilityPreset::None;
    m_presetPrefix.clear();
    if (m_repoPath.isEmpty() || m_storePath.isEmpty()) {
        return;
    }

    QJsonObject root = readTable(m_storePath);
    QJsonObject entry = root.value(m_repoPath).toObject();

    // A repository and its linked worktrees are keyed by the common git dir. Adopt
    // the old checkout-keyed entry once, so an upgrade does not read as "everything
    // is visible again".
    bool migrated = false;
    if (entry.isEmpty() && !legacyRepoPath.isEmpty() && legacyRepoPath != m_repoPath) {
        const QJsonObject legacy = root.value(legacyRepoPath).toObject();
        if (!legacy.isEmpty()) {
            entry = legacy;
            root.remove(legacyRepoPath);
            root.insert(m_repoPath, entry);
            migrated = true;
        }
    }

    const auto readList = [](const QJsonValue& value) {
        QStringList names;
        for (const QJsonValue& item : value.toArray()) {
            const QString name = item.toString();
            if (!name.isEmpty() && !names.contains(name)) {
                names.push_back(name);
            }
        }
        return names;
    };
    m_hidden = readList(entry.value(QStringLiteral("hidden")));
    m_solo = readList(entry.value(QStringLiteral("solo")));
    m_smart = entry.value(QStringLiteral("smart")).toBool();
    m_preset = presetFromId(entry.value(QStringLiteral("preset")).toString());
    m_presetPrefix = entry.value(QStringLiteral("prefix")).toString();

    if (migrated) {
        writeTable(m_storePath, root);
    }
}

bool RefVisibility::isHidden(const QString& name) const {
    return m_hidden.contains(name);
}

bool RefVisibility::isSolo(const QString& name) const {
    return m_solo.contains(name);
}

void RefVisibility::setHidden(const QString& name, bool hidden) {
    if (name.isEmpty()) {
        return;
    }
    const bool present = m_hidden.contains(name);
    if (hidden && !present) {
        m_hidden.push_back(name);
    } else if (!hidden && present) {
        m_hidden.removeAll(name);
    } else {
        return;
    }
    save();
}

void RefVisibility::setSolo(const QString& name, bool solo) {
    if (name.isEmpty()) {
        return;
    }
    const bool present = m_solo.contains(name);
    if (solo && !present) {
        m_solo.push_back(name);
    } else if (!solo && present) {
        m_solo.removeAll(name);
    } else {
        return;
    }
    save();
}

void RefVisibility::replaceHidden(const QStringList& names) {
    QStringList hidden = names;
    hidden.removeDuplicates();
    if (m_hidden == hidden && m_solo.isEmpty() && !m_smart && m_preset == VisibilityPreset::None) {
        return;
    }
    m_hidden = hidden;
    m_solo.clear();
    m_smart = false;
    m_preset = VisibilityPreset::None;
    save();
}

void RefVisibility::clear() {
    if (m_hidden.isEmpty() && m_solo.isEmpty() && !m_smart &&
        m_preset == VisibilityPreset::None) {
        return;
    }
    m_hidden.clear();
    m_solo.clear();
    m_smart = false;
    m_preset = VisibilityPreset::None;
    save();
}

void RefVisibility::setSmart(bool smart) {
    if (m_smart == smart && !(smart && m_preset != VisibilityPreset::None)) {
        return;
    }
    m_smart = smart;
    if (smart) {
        m_preset = VisibilityPreset::None;
    }
    save();
}

void RefVisibility::setPreset(VisibilityPreset preset) {
    if (m_preset == preset && !(preset != VisibilityPreset::None && m_smart)) {
        return;
    }
    m_preset = preset;
    if (preset != VisibilityPreset::None) {
        m_smart = false;
    }
    save();
}

void RefVisibility::setPresetPrefix(const QString& prefix) {
    if (m_presetPrefix == prefix) {
        return;
    }
    m_presetPrefix = prefix;
    save();
}

void RefVisibility::save() const {
    if (m_repoPath.isEmpty() || m_storePath.isEmpty()) {
        return;
    }

    QJsonObject root = readTable(m_storePath);

    if (m_hidden.isEmpty() && m_solo.isEmpty() && !m_smart &&
        m_preset == VisibilityPreset::None) {
        root.remove(m_repoPath);
    } else {
        QJsonObject entry;
        if (m_smart) {
            entry.insert(QStringLiteral("smart"), true);
        }
        if (m_preset != VisibilityPreset::None) {
            entry.insert(QStringLiteral("preset"), presetId(m_preset));
        }
        if (!m_presetPrefix.isEmpty()) {
            entry.insert(QStringLiteral("prefix"), m_presetPrefix);
        }
        if (!m_hidden.isEmpty()) {
            QJsonArray array;
            for (const QString& name : m_hidden) {
                array.append(name);
            }
            entry.insert(QStringLiteral("hidden"), array);
        }
        if (!m_solo.isEmpty()) {
            QJsonArray array;
            for (const QString& name : m_solo) {
                array.append(name);
            }
            entry.insert(QStringLiteral("solo"), array);
        }
        root.insert(m_repoPath, entry);
    }

    writeTable(m_storePath, root);
}

} // namespace gk
