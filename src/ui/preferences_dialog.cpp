#include "ui/preferences_dialog.h"

#include "ui/settings_store.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace gk {

PreferencesDialog::PreferencesDialog(const QString& repositoryPath, const QStringList& remoteNames,
                                     const RererePreferences& rerere, const QList<KeymapRow>& keymapRows,
                                     const QStringList& reservedShortcuts,
                                     const ExternalToolsPreferences& externalTools,
                                     const SigningPreferences& signing, QWidget* parent)
    : QDialog(parent),
      m_repositoryPath(repositoryPath),
      m_remoteNames(remoteNames),
      m_rerere(rerere),
      m_rerereEnabled(rerere.enabled),
      m_rerereAutoupdate(rerere.autoupdate),
      m_externalTools(externalTools),
      m_signing(signing),
      m_keymapRows(keymapRows),
      m_reservedShortcuts(reservedShortcuts) {
    setWindowTitle(QStringLiteral("Preferences"));
    setModal(true);
    resize(640, 560);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildAppearancePage(), QStringLiteral("Appearance"));
    const int graphTab = tabs->addTab(buildGraphPage(), QStringLiteral("Graph"));
    const int conflictsTab = tabs->addTab(buildConflictsPage(), QStringLiteral("Conflicts"));
    const int repositoryTab = tabs->addTab(buildRepositoryPage(), QStringLiteral("Repository"));
    const int signingTab = tabs->addTab(buildSigningPage(), QStringLiteral("Git"));
    const int toolsTab = tabs->addTab(buildToolsPage(), QStringLiteral("Tools"));
    const int keymapTab = tabs->addTab(buildKeymapPage(), QStringLiteral("Keymap"));
    (void)graphTab;
    (void)keymapTab;

    if (m_repositoryPath.isEmpty()) {
        tabs->setTabEnabled(conflictsTab, false);
        tabs->setTabToolTip(conflictsTab, QStringLiteral("Open a repository to change these"));
        tabs->setTabEnabled(repositoryTab, false);
        tabs->setTabToolTip(repositoryTab, QStringLiteral("Open a repository to change these"));
        tabs->setTabEnabled(signingTab, false);
        tabs->setTabToolTip(signingTab, QStringLiteral("Open a repository to change these"));
        tabs->setTabEnabled(toolsTab, false);
        tabs->setTabToolTip(toolsTab, QStringLiteral("Open a repository to change these"));
    }

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    // The write happens only once the choice is final: Cancel must leave the
    // profile exactly as it was.
    connect(this, &QDialog::accepted, this, &PreferencesDialog::save);
}

QWidget* PreferencesDialog::buildAppearancePage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* note = new QLabel(
        QStringLiteral("The theme and the text size apply as soon as you press OK."), page);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);

    auto* form = new QFormLayout;
    m_themeBox = new QComboBox(page);
    m_themeBox->setObjectName(QStringLiteral("theme"));
    m_themeBox->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
    m_themeBox->addItem(QStringLiteral("Light"), QStringLiteral("light"));
    const QString theme =
        SettingsStore().stringValue(SettingsStore::globalKey(QStringLiteral("theme")),
                                    QStringLiteral("dark"));
    m_themeBox->setCurrentIndex(theme.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0
                                    ? 1
                                    : 0);
    form->addRow(QStringLiteral("Theme"), m_themeBox);

    m_zoomBox = new QSpinBox(page);
    m_zoomBox->setObjectName(QStringLiteral("zoom"));
    m_zoomBox->setRange(50, 250);
    m_zoomBox->setSingleStep(10);
    m_zoomBox->setSuffix(QStringLiteral("%"));
    m_zoomBox->setValue(
        SettingsStore().intValue(SettingsStore::globalKey(QStringLiteral("zoom")), 100));
    form->addRow(QStringLiteral("Text size"), m_zoomBox);

    m_dateFormatBox = new QComboBox(page);
    m_dateFormatBox->setObjectName(QStringLiteral("dateFormat"));
    m_dateFormatBox->addItem(QStringLiteral("ISO (2026-01-31 14:05)"), QStringLiteral("iso"));
    m_dateFormatBox->addItem(QStringLiteral("European (31.01.2026 14:05)"),
                             QStringLiteral("european"));
    m_dateFormatBox->addItem(QStringLiteral("US (01/31/2026 2:05 PM)"), QStringLiteral("us"));
    m_dateFormatBox->addItem(QStringLiteral("Relative (3 h ago)"), QStringLiteral("relative"));
    const QString dateFormat =
        SettingsStore().stringValue(SettingsStore::globalKey(QStringLiteral("commit-date-format")),
                                    QStringLiteral("iso"));
    const int dateIndex = m_dateFormatBox->findData(dateFormat);
    m_dateFormatBox->setCurrentIndex(dateIndex >= 0 ? dateIndex : 0);
    form->addRow(QStringLiteral("Date format"), m_dateFormatBox);

    layout->addLayout(form);
    layout->addStretch(1);
    return page;
}

QWidget* PreferencesDialog::buildGraphPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    m_animateBox = new QCheckBox(QStringLiteral("Animate the commit graph"), page);
    m_animateBox->setObjectName(QStringLiteral("animateGraph"));
    m_animateBox->setToolTip(
        QStringLiteral("Morph rows when the graph changes. Off keeps every frame still."));
    m_animateBox->setChecked(
        SettingsStore().boolValue(SettingsStore::globalKey(QStringLiteral("graph/animate")), true));
    layout->addWidget(m_animateBox);
    layout->addStretch(1);
    return page;
}

QWidget* PreferencesDialog::buildConflictsPage() {
    auto* store = new SettingsStore(m_repositoryPath);
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    m_preflightBox = new QCheckBox(QStringLiteral("Check for conflicts before merging"), page);
    m_preflightBox->setObjectName(QStringLiteral("preflight"));
    m_preflightBox->setToolTip(QStringLiteral(
        "Predict a merge with git merge-tree and warn before the working tree is touched."));
    m_preflightBox->setChecked(store->boolValue(store->repositoryKey(QStringLiteral("preflight")),
                                                false));
    layout->addWidget(m_preflightBox);

    layout->addSpacing(8);
    m_rerereBox = new QCheckBox(QStringLiteral("Remember conflict resolutions (rerere)"), page);
    m_rerereBox->setObjectName(QStringLiteral("rerereEnabled"));
    m_rerereBox->setEnabled(m_rerere.available);
    m_rerereBox->setChecked(m_rerereEnabled);
    layout->addWidget(m_rerereBox);

    m_rerereAutoupdateBox = new QCheckBox(QStringLiteral("  …and stage them automatically"), page);
    m_rerereAutoupdateBox->setObjectName(QStringLiteral("rerereAutoupdate"));
    m_rerereAutoupdateBox->setEnabled(m_rerere.available && m_rerereEnabled);
    m_rerereAutoupdateBox->setChecked(m_rerereAutoupdate);
    layout->addWidget(m_rerereAutoupdateBox);

    connect(m_rerereBox, &QCheckBox::toggled, this, [this](bool on) {
        m_rerereEnabled = on;
        m_rerereAutoupdateBox->setEnabled(on);
    });
    connect(m_rerereAutoupdateBox, &QCheckBox::toggled, this,
            [this](bool on) { m_rerereAutoupdate = on; });

    if (!m_rerere.available) {
        auto* hint = new QLabel(QStringLiteral("Open a repository to change these."), page);
        hint->setEnabled(false);
        layout->addWidget(hint);
    }
    layout->addStretch(1);
    return page;
}

QWidget* PreferencesDialog::buildRepositoryPage() {
    auto* store = new SettingsStore(m_repositoryPath);
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* fetchBox = new QGroupBox(QStringLiteral("Auto-fetch"), page);
    auto* fetchLayout = new QVBoxLayout(fetchBox);
    m_autoFetchBox = new QCheckBox(QStringLiteral("Fetch every 5 minutes"), fetchBox);
    m_autoFetchBox->setObjectName(QStringLiteral("autoFetchEnabled"));
    fetchLayout->addWidget(m_autoFetchBox);

    const QStringList watched =
        store->listValue(store->repositoryKey(QStringLiteral("autofetch")) + QStringLiteral("/remotes"));
    const bool watchAll = watched.isEmpty();
    m_autoFetchAllBox = new QCheckBox(QStringLiteral("Watch every remote"), fetchBox);
    m_autoFetchAllBox->setObjectName(QStringLiteral("autoFetchAll"));
    m_autoFetchAllBox->setChecked(watchAll);
    fetchLayout->addWidget(m_autoFetchAllBox);

    m_autoFetchRemotesList = new QListWidget(fetchBox);
    m_autoFetchRemotesList->setObjectName(QStringLiteral("autoFetchRemotes"));
    for (const QString& remote : m_remoteNames) {
        auto* item = new QListWidgetItem(remote, m_autoFetchRemotesList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(watchAll || watched.contains(remote) ? Qt::Checked : Qt::Unchecked);
    }
    m_autoFetchRemotesList->setEnabled(!watchAll && !m_remoteNames.isEmpty());
    fetchLayout->addWidget(m_autoFetchRemotesList);
    if (m_remoteNames.isEmpty()) {
        auto* none = new QLabel(QStringLiteral("No remotes are configured."), fetchBox);
        none->setEnabled(false);
        fetchLayout->addWidget(none);
    }
    layout->addWidget(fetchBox);

    auto* submoduleBox = new QGroupBox(QStringLiteral("Submodules"), page);
    auto* submoduleLayout = new QVBoxLayout(submoduleBox);
    m_submoduleBox = new QCheckBox(
        QStringLiteral("Update after fetch, checkout or a finished merge"), submoduleBox);
    m_submoduleBox->setObjectName(QStringLiteral("submoduleAutoupdate"));
    m_submoduleBox->setChecked(
        store->boolValue(store->repositoryKey(QStringLiteral("submodules")) + QStringLiteral("/enabled"),
                         false));
    submoduleLayout->addWidget(m_submoduleBox);
    m_submoduleRemoteBox =
        new QCheckBox(QStringLiteral("  …tracking the submodule's remote branch"), submoduleBox);
    m_submoduleRemoteBox->setObjectName(QStringLiteral("submoduleAutoupdateRemote"));
    m_submoduleRemoteBox->setChecked(
        store->boolValue(store->repositoryKey(QStringLiteral("submodules")) + QStringLiteral("/remote"),
                         false));
    submoduleLayout->addWidget(m_submoduleRemoteBox);
    layout->addWidget(submoduleBox);

    connect(m_autoFetchAllBox, &QCheckBox::toggled, this, [this](bool all) {
        m_autoFetchRemotesList->setEnabled(!all && !m_remoteNames.isEmpty());
    });
    connect(m_submoduleBox, &QCheckBox::toggled, this,
            [this](bool on) { m_submoduleRemoteBox->setEnabled(on); });
    m_submoduleRemoteBox->setEnabled(m_submoduleBox->isChecked());

    layout->addStretch(1);
    return page;
}

QWidget* PreferencesDialog::buildSigningPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* note = new QLabel(
        QStringLiteral("These live in this repository's git config. Signing runs through "
                       "git, so your gpg or ssh setup, key and agent are what apply."),
        page);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);

    m_signCommitBox = new QCheckBox(QStringLiteral("Sign commits by default"), page);
    m_signCommitBox->setObjectName(QStringLiteral("signCommits"));
    m_signCommitBox->setChecked(m_signing.sign);
    layout->addWidget(m_signCommitBox);

    auto* form = new QFormLayout;

    m_signingKeyEdit = new QLineEdit(m_signing.signingKey, page);
    m_signingKeyEdit->setObjectName(QStringLiteral("signingKey"));
    m_signingKeyEdit->setPlaceholderText(QStringLiteral("key id, fingerprint or key file"));
    form->addRow(QStringLiteral("Signing key (user.signingkey)"), m_signingKeyEdit);

    m_signingFormatBox = new QComboBox(page);
    m_signingFormatBox->setObjectName(QStringLiteral("signingFormat"));
    m_signingFormatBox->addItem(QStringLiteral("git default"), QString());
    m_signingFormatBox->addItem(QStringLiteral("OpenPGP"), QStringLiteral("openpgp"));
    m_signingFormatBox->addItem(QStringLiteral("SSH"), QStringLiteral("ssh"));
    const int formatIndex = m_signingFormatBox->findData(m_signing.format);
    m_signingFormatBox->setCurrentIndex(formatIndex >= 0 ? formatIndex : 0);
    form->addRow(QStringLiteral("Format (gpg.format)"), m_signingFormatBox);

    m_gpgProgramEdit = new QLineEdit(m_signing.gpgProgram, page);
    m_gpgProgramEdit->setObjectName(QStringLiteral("gpgProgram"));
    m_gpgProgramEdit->setPlaceholderText(QStringLiteral("gpg"));
    form->addRow(QStringLiteral("GPG program (gpg.program)"), m_gpgProgramEdit);

    layout->addLayout(form);
    layout->addStretch(1);
    return page;
}

QWidget* PreferencesDialog::buildToolsPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* note = new QLabel(
        QStringLiteral("The tool must be installed and on your PATH; git invokes it. The "
                       "choice is written into this repository's git config. Leave it on "
                       "\"git default\" to let git pick."),
        page);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);

    auto* form = new QFormLayout;

    m_diffToolBox = new QComboBox(page);
    m_diffToolBox->setObjectName(QStringLiteral("diffTool"));
    m_diffToolBox->setEditable(true);
    m_diffToolBox->addItem(QStringLiteral("git default"), QString());
    for (const QString& name : m_externalTools.diffTools) {
        m_diffToolBox->addItem(name, name);
    }
    if (m_externalTools.diffTool.isEmpty()) {
        m_diffToolBox->setCurrentIndex(0);
    } else {
        m_diffToolBox->setCurrentText(m_externalTools.diffTool);
    }
    form->addRow(QStringLiteral("Diff tool (diff.tool)"), m_diffToolBox);

    m_mergeToolBox = new QComboBox(page);
    m_mergeToolBox->setObjectName(QStringLiteral("mergeTool"));
    m_mergeToolBox->setEditable(true);
    m_mergeToolBox->addItem(QStringLiteral("git default"), QString());
    for (const QString& name : m_externalTools.mergeTools) {
        m_mergeToolBox->addItem(name, name);
    }
    if (m_externalTools.mergeTool.isEmpty()) {
        m_mergeToolBox->setCurrentIndex(0);
    } else {
        m_mergeToolBox->setCurrentText(m_externalTools.mergeTool);
    }
    form->addRow(QStringLiteral("Merge tool (merge.tool)"), m_mergeToolBox);

    layout->addLayout(form);
    layout->addStretch(1);
    return page;
}

QString PreferencesDialog::diffTool() const {
    if (!m_diffToolBox) {
        return {};
    }
    const QString text = m_diffToolBox->currentText().trimmed();
    return text == QStringLiteral("git default") ? QString() : text;
}

QString PreferencesDialog::mergeTool() const {
    if (!m_mergeToolBox) {
        return {};
    }
    const QString text = m_mergeToolBox->currentText().trimmed();
    return text == QStringLiteral("git default") ? QString() : text;
}

bool PreferencesDialog::signCommits() const {
    return m_signCommitBox && m_signCommitBox->isChecked();
}

QString PreferencesDialog::signingKey() const {
    return m_signingKeyEdit ? m_signingKeyEdit->text().trimmed() : QString();
}

QString PreferencesDialog::signingFormat() const {
    return m_signingFormatBox ? m_signingFormatBox->currentData().toString() : QString();
}

QString PreferencesDialog::gpgProgram() const {
    return m_gpgProgramEdit ? m_gpgProgramEdit->text().trimmed() : QString();
}

QWidget* PreferencesDialog::buildKeymapPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* note = new QLabel(
        QStringLiteral("Click a shortcut and press a new combination. An empty field restores the "
                       "command's default; changes apply when you press OK."),
        page);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);

    m_keymapTree = new QTreeWidget(page);
    m_keymapTree->setColumnCount(2);
    m_keymapTree->setHeaderLabels({QStringLiteral("Command"), QStringLiteral("Shortcut")});
    m_keymapTree->setRootIsDecorated(false);
    m_keymapTree->setUniformRowHeights(true);
    m_keymapTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_keymapTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    for (const KeymapRow& row : m_keymapRows) {
        auto* item = new QTreeWidgetItem(m_keymapTree);
        item->setText(0, row.category.isEmpty()
                             ? row.title
                             : QStringLiteral("%1: %2").arg(row.category, row.title));
        item->setData(0, Qt::UserRole, row.id);
        auto* edit = new QKeySequenceEdit(row.sequence, m_keymapTree);
        edit->setObjectName(row.id);
        edit->setClearButtonEnabled(true);
        m_keymapTree->setItemWidget(item, 1, edit);
        m_keymapEditors.append(edit);
    }
    layout->addWidget(m_keymapTree, 1);
    return page;
}

QString PreferencesDialog::keymapProblem() {
    // Pull the edited sequences in first: the rows are the dialog's own record, and
    // the readers below are the widgets the editor left behind.
    for (int i = 0; i < m_keymapRows.size() && i < m_keymapEditors.size(); ++i) {
        m_keymapRows[i].sequence = m_keymapEditors.at(i)->keySequence();
    }
    for (int i = 0; i < m_keymapRows.size(); ++i) {
        const QKeySequence sequence = m_keymapRows.at(i).sequence;
        if (sequence.isEmpty()) {
            continue;
        }
        const QString text = sequence.toString(QKeySequence::PortableText);
        if (m_reservedShortcuts.contains(text)) {
            return QStringLiteral("%1 is used by the window itself.").arg(text);
        }
        for (int j = i + 1; j < m_keymapRows.size(); ++j) {
            if (m_keymapRows.at(j).sequence.toString(QKeySequence::PortableText) == text) {
                return QStringLiteral("%1 is already bound to \"%2\".")
                    .arg(text, m_keymapRows.at(j).title);
            }
        }
    }
    return {};
}

void PreferencesDialog::accept() {
    // Refuse a clash before anything is written: the dialog staying open is the only
    // way to tell the reader which pair collides.
    const QString problem = keymapProblem();
    if (!problem.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Cannot save shortcuts"), problem);
        return;
    }
    QDialog::accept();
}

void PreferencesDialog::save() {
    SettingsStore store(m_repositoryPath);
    store.setBool(SettingsStore::globalKey(QStringLiteral("graph/animate")),
                  m_animateBox->isChecked());
    store.setString(SettingsStore::globalKey(QStringLiteral("theme")),
                    m_themeBox->currentData().toString());
    store.setInt(SettingsStore::globalKey(QStringLiteral("zoom")), m_zoomBox->value());
    store.setString(SettingsStore::globalKey(QStringLiteral("commit-date-format")),
                    m_dateFormatBox->currentData().toString());
    if (m_repositoryPath.isEmpty()) {
        return;
    }

    store.setBool(store.repositoryKey(QStringLiteral("preflight")), m_preflightBox->isChecked());

    store.setBool(store.repositoryKey(QStringLiteral("autofetch")) + QStringLiteral("/enabled"),
                  m_autoFetchBox->isChecked());
    // Empty means "every remote", which is what the menu already stores when the
    // reader names all of them; keep the two spellings the same value.
    QStringList chosen;
    if (!m_autoFetchAllBox->isChecked()) {
        for (int i = 0; i < m_autoFetchRemotesList->count(); ++i) {
            QListWidgetItem* item = m_autoFetchRemotesList->item(i);
            if (item->checkState() == Qt::Checked) {
                chosen << item->text();
            }
        }
    }
    store.setList(store.repositoryKey(QStringLiteral("autofetch")) + QStringLiteral("/remotes"),
                  chosen);

    store.setBool(store.repositoryKey(QStringLiteral("submodules")) + QStringLiteral("/enabled"),
                  m_submoduleBox->isChecked());
    store.setBool(store.repositoryKey(QStringLiteral("submodules")) + QStringLiteral("/remote"),
                  m_submoduleRemoteBox->isChecked());
    store.sync();
}

} // namespace gk
