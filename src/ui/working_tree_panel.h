#pragma once

#include "core/types.h"
#include "model/commit_files_model.h"

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QToolButton;
class QTreeView;

namespace gk {

class Skeleton;

class FileActionDelegate;

class WorkingTreePanel : public QWidget {
    Q_OBJECT

public:
    explicit WorkingTreePanel(QWidget* parent = nullptr);

    void setChanges(const std::vector<FileChange>& changes);
    void setLoading(bool loading);
    bool loading() const;
    void setBranch(const QString& name, const QColor& color);
    void setMode(CommitFilesModel::Mode mode);

    QStringList selectedPaths() const;
    QString firstFilePath() const;
    bool firstFileIsStaged() const;
    void selectFirstFile();
    bool selectPath(const QString& path, bool staged);

    /// The summary, then a blank line and the description when there is one —
    /// exactly what is committed.
    QString commitMessage() const;
    void setCommitMessage(const QString& message);
    void clearCommitMessage();
    void focusCommitMessage();
    QString stashMessage() const;
    void clearStashMessage();

    bool signCommit() const;
    void setSignCommit(bool on);
    bool amend() const;
    /// Unticks "Amend previous commit" without putting a draft back, after the
    /// amend has been made.
    void resetAmend();
    bool pushAfterCommit() const;

    int stagedCount() const;
    QStringList stagedPaths() const;
    int unstagedCount() const;
    int changedCount() const;

    void refreshTheme();

signals:
    void fileSelected(QString path, bool staged);
    void fileActivated(QString path, bool staged);

    void stageRequested(QStringList paths);
    void unstageRequested(QStringList paths);
    void discardRequested(QStringList paths);
    void stashRequested(QStringList paths);
    void ignoreRequested(QStringList paths, QString scope, QString kind);
    void stageAllRequested();
    void unstageAllRequested();
    void discardAllRequested();

    void commitRequested(QString message);
    /// "Amend previous commit" was ticked: the window fills the message with HEAD's.
    /// Unticking puts back what was typed before, here, without asking anyone.
    void amendToggled(bool on);
    void stashAllRequested(QString message);
    void signChanged(bool on);
    void coauthorsRequested();

private:
    struct Section {
        QToolButton* toggle = nullptr;
        QPushButton* all = nullptr;
        QTreeView* view = nullptr;
        CommitFilesModel* model = nullptr;
        FileActionDelegate* action = nullptr;
        bool staged = false;
    };

    void buildUi();
    QWidget* buildHeader();
    QWidget* buildFileToolbar();
    QWidget* buildFiles();
    QWidget* buildForms();
    QWidget* buildCommitForm();
    QWidget* buildStashForm();
    void buildSection(Section& section, QWidget* parent, bool staged);
    void wireSection(Section& section);
    void showContextMenu(Section& section, const QPoint& pos);

    void refreshCounts();
    void refreshSectionState(Section& section);
    void refreshCommitButton();
    void refreshStashButton();
    void applySort();
    QStringList selectedPathsIn(const Section& section) const;
    QModelIndex firstFileIn(const Section& section) const;
    Section& other(const Section& section);

    Skeleton* m_skeleton = nullptr;
    Section m_unstaged;
    Section m_staged;
    int m_changedPaths = 0;

    QPushButton* m_discardAll = nullptr;
    QLabel* m_countLabel = nullptr;
    QLabel* m_branchPill = nullptr;
    QString m_branchName;
    QColor m_branchColor;

    QToolButton* m_sort = nullptr;
    bool m_descending = false;
    QToolButton* m_pathMode = nullptr;
    QToolButton* m_treeMode = nullptr;

    QTabWidget* m_tabs = nullptr;
    QCheckBox* m_amend = nullptr;
    QString m_draftBeforeAmend;
    QLineEdit* m_summary = nullptr;
    QPlainTextEdit* m_description = nullptr;
    QToolButton* m_optionsToggle = nullptr;
    QWidget* m_options = nullptr;
    QCheckBox* m_pushAfter = nullptr;
    QCheckBox* m_signCommit = nullptr;
    QPushButton* m_coauthors = nullptr;
    QPushButton* m_commit = nullptr;

    QLineEdit* m_stashTitle = nullptr;
    QPlainTextEdit* m_stashDescription = nullptr;
    QPushButton* m_stash = nullptr;
};

} // namespace gk
