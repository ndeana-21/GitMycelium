#include "model/ref_pins.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace gk {
namespace {

/// The whole per-repository table, or an empty object when nothing is stored yet.
/// Read before every write: the file holds every other repository's pins.
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
    // the next launch would then silently lose every pin in every repository.
    QSaveFile out(storePath);
    if (!out.open(QIODevice::WriteOnly)) {
        return;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.commit();
}

} // namespace

QString RefPins::defaultStorePath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/pins.json");
}

RefPins::RefPins() : m_storePath(defaultStorePath()) {}

RefPins::RefPins(QString storePath) : m_storePath(std::move(storePath)) {}

void RefPins::load(const QString& repoPath, const QString& legacyRepoPath) {
    m_repoPath = repoPath;
    m_names.clear();
    if (m_repoPath.isEmpty() || m_storePath.isEmpty()) {
        return;
    }

    QJsonObject root = readTable(m_storePath);
    QJsonArray stored = root.value(m_repoPath).toArray();

    // A repository and its linked worktrees are keyed by the common git dir. Adopt
    // the old checkout-keyed entry once, so an upgrade does not read as "you have
    // no pins".
    bool migrated = false;
    if (stored.isEmpty() && !legacyRepoPath.isEmpty() && legacyRepoPath != m_repoPath) {
        const QJsonArray legacy = root.value(legacyRepoPath).toArray();
        if (!legacy.isEmpty()) {
            stored = legacy;
            root.remove(legacyRepoPath);
            root.insert(m_repoPath, stored);
            migrated = true;
        }
    }

    for (const QJsonValue& value : stored) {
        const QString name = value.toString();
        if (!name.isEmpty() && !m_names.contains(name)) {
            m_names.push_back(name);
        }
    }
    if (migrated) {
        writeTable(m_storePath, root);
    }
}

bool RefPins::isPinned(const QString& name) const {
    return m_names.contains(name);
}

void RefPins::setPinned(const QString& name, bool pinned) {
    if (name.isEmpty()) {
        return;
    }
    const bool present = m_names.contains(name);
    if (pinned && !present) {
        m_names.push_back(name);
    } else if (!pinned && present) {
        m_names.removeAll(name);
    } else {
        return;
    }
    save();
}

void RefPins::clear() {
    if (m_names.isEmpty()) {
        return;
    }
    m_names.clear();
    save();
}

void RefPins::save() const {
    if (m_repoPath.isEmpty() || m_storePath.isEmpty()) {
        return;
    }

    QJsonObject root = readTable(m_storePath);
    if (m_names.isEmpty()) {
        root.remove(m_repoPath);
    } else {
        QJsonArray array;
        for (const QString& name : m_names) {
            array.append(name);
        }
        root.insert(m_repoPath, array);
    }
    writeTable(m_storePath, root);
}

} // namespace gk
