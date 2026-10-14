#pragma once

#include <QString>
#include <QStringList>

namespace gk {

/// One identity: the name and email a commit is authored with, and an optional
/// signing key. A profile is a person, not a repository, so the definitions are
/// global; *applying* one writes it into the current repository's local config, so
/// choosing "Work" here never changes what another repository commits as.
struct GitProfile {
    QString name;
    QString userName;
    QString email;
    QString signingKey;
    bool sign = false;
};

namespace profiles {

QStringList names();
GitProfile load(const QString& name);
/// Adds or replaces a profile. An empty name is refused by the caller, not here.
void save(const GitProfile& profile);
void remove(const QString& name);

QString activeFor(const QString& repositoryPath);
void setActiveFor(const QString& repositoryPath, const QString& name);

} // namespace profiles
} // namespace gk
