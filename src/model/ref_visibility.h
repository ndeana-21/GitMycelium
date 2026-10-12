#pragma once

#include <QString>
#include <QStringList>

namespace gk {

/// A saved branch-filter preset, remembered per repository.
///
/// Unlike hide/solo this is a *mode*, not a snapshot: the visible set is recomputed
/// from the refs on every refresh, so a branch created later that still matches —
/// another `feature/x`, another commit by the same author — shows up on its own.
enum class VisibilityPreset {
    None,
    Mine,       ///< the tip was authored by the configured user
    Recent30,   ///< the tip commit is younger than 30 days
    Prefix,     ///< the branch name starts with a stored prefix (`feature/`)
    NoUpstream, ///< a local branch that tracks nothing
    Ahead,      ///< commits the upstream lacks (ready to push)
    Behind,     ///< commits the upstream has that the branch lacks
};

/// Which references the graph shows, remembered per repository.
///
/// Like `RefPins`, this lives outside the models: the ref model and the commit
/// model are rebuilt on every fetch and every keystroke, while hiding a branch is
/// a decision the reader made once. Keyed by the repository's canonical path.
///
/// The stored names are short branch names (`main`, `origin/main`). Solo wins
/// over hide: asking to see one branch should not also show the others. HEAD is
/// never filtered — a graph that hides the branch you are on is not a graph —
/// and that rule lives where the filter is built, not here.
class RefVisibility {
public:
    RefVisibility();
    /// Uses `storePath` instead — how a test gets its own file.
    explicit RefVisibility(QString storePath);

    /// Loads the visibility recorded for `repoPath`. A missing or unreadable file
    /// is not an error: it means nothing has been hidden yet. `legacyRepoPath`,
    /// when given, is the old checkout-path key (before entries moved to the common
    /// git dir); a non-empty entry there is adopted.
    void load(const QString& repoPath, const QString& legacyRepoPath = QString());

    bool isHidden(const QString& name) const;
    bool isSolo(const QString& name) const;
    void setHidden(const QString& name, bool hidden);
    void setSolo(const QString& name, bool solo);

    /// Focus on the current branch: show the current branch, the repository's target
    /// and their upstreams, and hide the rest. It is a mode, not a snapshot — the
    /// set is recomputed from the refs on every refresh.
    bool smart() const { return m_smart; }
    void setSmart(bool smart);

    /// The active filter preset. Setting any preset turns smart off (they are two
    /// ways to answer the same question), and turning smart on clears the preset.
    VisibilityPreset preset() const { return m_preset; }
    void setPreset(VisibilityPreset preset);
    /// The name fragment the Prefix preset matches, e.g. `feature/`.
    const QString& presetPrefix() const { return m_presetPrefix; }
    void setPresetPrefix(const QString& prefix);

    bool soloActive() const { return !m_solo.isEmpty(); }
    bool anyHidden() const { return !m_hidden.isEmpty(); }
    bool empty() const {
        return m_hidden.isEmpty() && m_solo.isEmpty() && !m_smart &&
               m_preset == VisibilityPreset::None;
    }

    const QStringList& hiddenNames() const { return m_hidden; }
    const QStringList& soloNames() const { return m_solo; }

    void clear();

    static QString defaultStorePath();

private:
    void save() const;

    QString m_storePath;
    QString m_repoPath;
    QStringList m_hidden;
    QStringList m_solo;
    bool m_smart = false;
    VisibilityPreset m_preset = VisibilityPreset::None;
    QString m_presetPrefix;
};

} // namespace gk
