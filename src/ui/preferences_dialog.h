#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTreeWidget;

namespace gk {

/// rerere's state as the dialog found it. rerere lives in the git config rather
/// than in `QSettings`, so the dialog only reports the reader's choice and the
/// window applies it through the session afterwards.
struct RererePreferences {
    bool available = false;
    bool enabled = false;
    bool autoupdate = false;
};

/// One bindable command in the Keymap page: what to call it, the sequence it ships
/// with, and the sequence it has now. The dialog edits `sequence`; the window turns
/// the difference back into an override.
struct KeymapRow {
    QString id;
    QString title;
    QString category;
    QKeySequence defaultSequence;
    QKeySequence sequence;
};

struct ExternalToolsPreferences {
    bool available = false;
    QString diffTool;
    QString mergeTool;
    QStringList diffTools;
    QStringList mergeTools;
};

struct SigningPreferences {
    bool available = false;
    bool sign = false;
    QString signingKey;
    QString format; ///< "openpgp", "ssh", or empty for git's default
    QString gpgProgram;
};

/// Settings that outlive one operation: which way the window looks and what it
/// does without asking. Values are read from `QSettings` when the dialog opens
/// and written back on accept; the window re-reads and applies them afterwards,
/// so `QSettings` stays the one source of truth. Repository-scoped pages are
/// disabled while no repository is open: a setting with no repository to key it
/// by would be a setting nobody can find again.
class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    PreferencesDialog(const QString& repositoryPath, const QStringList& remoteNames,
                      const RererePreferences& rerere, const QList<KeymapRow>& keymapRows = {},
                      const QStringList& reservedShortcuts = {},
                      const ExternalToolsPreferences& externalTools = {},
                      const SigningPreferences& signing = {}, QWidget* parent = nullptr);

    bool rerereEnabled() const { return m_rerereEnabled; }
    bool rerereAutoupdate() const { return m_rerereAutoupdate; }
    QString diffTool() const;
    QString mergeTool() const;
    bool signCommits() const;
    QString signingKey() const;
    QString signingFormat() const;
    QString gpgProgram() const;
    /// The keymap rows with their edited sequences. The window compares each to its
    /// default and stores an override only where it differs.
    QList<KeymapRow> keymapRows() const { return m_keymapRows; }

    /// Validates the keymap before the dialog closes: a duplicate or a reserved
    /// combination is refused here, so a profile is never left with two commands
    /// fighting over one key. Public because it overrides a public slot.
    void accept() override;

    /// Reads the edited sequences and returns the first problem, or empty. Exposed
    /// so the check can be asserted without a modal dialog standing in the way.
    QString keymapProblem();

private:
    QWidget* buildAppearancePage();
    QWidget* buildGraphPage();
    QWidget* buildConflictsPage();
    QWidget* buildRepositoryPage();
    QWidget* buildSigningPage();
    QWidget* buildToolsPage();
    QWidget* buildKeymapPage();
    void save();

    QString m_repositoryPath;
    QStringList m_remoteNames;
    RererePreferences m_rerere;
    bool m_rerereEnabled = false;
    bool m_rerereAutoupdate = false;
    ExternalToolsPreferences m_externalTools;
    SigningPreferences m_signing;

    QCheckBox* m_animateBox = nullptr;
    QCheckBox* m_foldMergedBox = nullptr;
    QCheckBox* m_mergedRemoteLabelsBox = nullptr;
    QCheckBox* m_updateCheckBox = nullptr;
    QComboBox* m_themeBox = nullptr;
    QSpinBox* m_zoomBox = nullptr;
    QComboBox* m_dateFormatBox = nullptr;
    QCheckBox* m_preflightBox = nullptr;
    QCheckBox* m_rerereBox = nullptr;
    QCheckBox* m_rerereAutoupdateBox = nullptr;
    QCheckBox* m_autoFetchBox = nullptr;
    QCheckBox* m_autoFetchAllBox = nullptr;
    QListWidget* m_autoFetchRemotesList = nullptr;
    QCheckBox* m_submoduleBox = nullptr;
    QCheckBox* m_submoduleRemoteBox = nullptr;
    QCheckBox* m_signCommitBox = nullptr;
    QLineEdit* m_signingKeyEdit = nullptr;
    QComboBox* m_signingFormatBox = nullptr;
    QLineEdit* m_gpgProgramEdit = nullptr;
    QComboBox* m_diffToolBox = nullptr;
    QComboBox* m_mergeToolBox = nullptr;

    QList<KeymapRow> m_keymapRows;
    QStringList m_reservedShortcuts;
    QTreeWidget* m_keymapTree = nullptr;
    QList<QKeySequenceEdit*> m_keymapEditors;
};

} // namespace gk
