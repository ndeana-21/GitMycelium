#pragma once

#include "core/conflict_regions.h"
#include "core/types.h"

#include <QPair>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QWidget>

#include <vector>

class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QTextEdit;
class QToolButton;
class QTreeWidget;

namespace gk {

class RepoSession;

/// Three-pane merge editor: Ours | Base | Theirs on top, the editable Result
/// below. This is the feature the project exists for, so it lives in the main
/// window as its own page rather than behind a dialog.
///
/// The Result starts as cards, one per conflict region git marked up, each with
/// its own choice of side; a region nobody has answered yet keeps its markers, so
/// saving never silently drops text. Manual editing is one toggle away.
class ConflictView : public QWidget {
    Q_OBJECT

public:
    explicit ConflictView(RepoSession* session, QWidget* parent = nullptr);

    /// Re-points the view at another repository. The view is shared across the
    /// window's repository tabs, so a tab switch hands it a different session.
    void setSession(RepoSession* session);

    /// Re-reads the conflict list and selects the first unresolved file.
    void reload();

    /// Re-renders the current file after a theme or zoom change: the panes and
    /// cards are HTML that carries the theme's colours and monospace size.
    void refreshTheme();
    bool hasConflicts() const { return !m_conflicts.empty(); }
    int conflictCount() const { return static_cast<int>(m_conflicts.size()); }
    /// Files the open operation has already resolved, listed under the conflicted
    /// ones so a resolution can be taken back. Empty when no operation is open.
    QStringList resolvedPaths() const { return m_resolvedPaths; }
    /// Puts a resolved file's conflict back in the index ("Mark as conflicted"); the
    /// working-tree file is left as it is.
    bool markConflicted(const QString& path, QString* errorOut);

    int regionCount() const { return static_cast<int>(m_document.regions.size()); }
    /// Regions the reader has not chosen a side for (broken blocks count too).
    int unresolvedRegionCount() const;
    int currentRegion() const { return m_currentRegion; }
    /// Chooses a side for one region: `ours`, `base`, `theirs`, `both`,
    /// `both-rev`, `lines`, or `clear`. False for an unknown index or choice.
    bool setRegionChoice(int region, const QString& choice);
    /// Takes or drops one line of one side, line by line — the per-line answer a
    /// whole-side chip cannot give. `side` is "ours" or "theirs". The region's
    /// choice becomes `lines`; taking the last line away leaves it unanswered.
    bool setRegionLine(int region, const QString& side, int line, bool take);
    bool takeAllRegionLines(int region, const QString& side, bool take);
    bool setRegionLineOrder(int region, bool oursFirst);
    int regionLineTakeCount(int region, const QString& side) const;
    /// Forgets every answer given in the current file — sides, lines and order —
    /// leaving each region unanswered.
    void clearChoices();
    /// The file as the current choices would write it, markers kept where a
    /// region is unresolved.
    QString assembledText() const;
    /// What a save would write: the card choices, or the manual editor's text.
    QString resultText() const;
    /// Selects a conflicted file by path. False when it is not conflicted.
    bool selectFile(const QString& path);
    QString currentPath() const { return m_path; }
    /// Writes and stages the result. Refuses while conflict markers remain.
    bool trySaveResolution(QString* errorOut);
    /// Keeps one whole index stage — the resolution for a binary file, where
    /// there is no text to merge. `side` is "ours", "base" or "theirs".
    bool keepSide(const QString& side, QString* errorOut);
    /// Merges a JSON conflict key by key. Fully decided keys are written out;
    /// disagreeing keys become the only regions left. False when the file is not a
    /// lone JSON object on every side.
    bool smartMerge(QString* errorOut);
    /// Stages the file as it is on disk — how a reader who resolved it in an
    /// external editor finishes. Refuses while markers remain.
    bool stageWorkingTree(QString* errorOut);
    /// Stages every conflicted file whose working-tree copy no longer carries
    /// markers. A file that still has them is left alone and named in `skippedOut`,
    /// because staging markers is how a merge "completes" with `<<<<<<<` in it.
    bool markAllResolved(QStringList* skippedOut, QString* errorOut);
    /// Settles the file on the deleting side: removes it from the index and the
    /// working tree. The right answer for a delete/modify conflict, and the only
    /// one that leaves no empty tracked file behind.
    bool deleteCurrentFile(QString* errorOut);
    /// Settles a delete/modify conflict with one of its four answers: "modified"
    /// keeps the side that changed the file, "base" the version both sides started
    /// from, "disk" the file as the working tree holds it now, and "delete" the
    /// deletion. False for any other conflict or answer.
    bool resolveDeleteModify(const QString& answer, QString* errorOut);
    /// The side that kept and changed the file in a delete/modify conflict:
    /// "ours" or "theirs", empty for any other conflict.
    QString deleteModifyLivingSide() const;
    /// Saves the current result to the working tree as a draft, markers and all,
    /// without staging it. The conflict stays open, so a partial resolution can be
    /// put on disk and picked up later.
    bool saveDraft(QString* errorOut);
    /// True when the current conflict is a delete/modify (one side removed the
    /// file). The editor then offers the explicit delete alongside the sides.
    bool isDeleteModify() const { return m_deleteModify; }
    QString currentKind() const;
    /// True when the file is binary (or its stages yield no text): the editor
    /// shows the sides' sizes and only whole-side choices are offered.
    bool isBinary() const { return m_binary; }
    QStringList describeRegions() const;

    void goToNextRegion();
    void goToPreviousRegion();

    void setManualMode(bool manual);
    bool manualMode() const { return m_manualMode; }

    /// True when rerere has a recorded resolution for the current file, and true
    /// again when it has already applied one to the working tree (the file is
    /// resolved on disk while the index still carries the conflict).
    bool rerereRemembered() const { return m_rerereRemembered; }
    bool rerereApplied() const { return m_rerereApplied; }
    bool stageRecordedResolution(QString* errorOut);
    /// Finds `text` in the focused pane (the result when none has focus).
    bool findInPanes(const QString& text, bool forward, bool caseSensitive);

signals:
    void repositoryChanged();
    void mergeCompleted();
    void mergeAborted();
    /// The reader asked to leave the merge editor. The page is the window's, not
    /// the view's, so the view asks rather than hides itself.
    void closeRequested();
    void externalMergeRequested(QString path);

private slots:
    void onFileSelected();
    void onRegionClicked(const QUrl& url);
    void onSaveClicked();
    void completeMerge();
    void abortMerge();
    void onToggleManual(bool manual);
    void onSyncScrollToggled(bool on);
    void onRerereClicked();
    void onSmartMerge();
    void onStageWorkingTree();
    void onFileContextMenu(const QPoint& pos);
    void onResolvedContextMenu(const QPoint& pos);
    void reloadResolved();
    void findNext();
    void findPrevious();

private:
    void showEntry(int index);
    /// Reads `path` from the working tree and stages it if it is resolved. Used by
    /// both the single-file and the all-files paths, so the marker check cannot be
    /// skipped in one of them.
    bool stagePath(const QString& path, QString* errorOut);
    QString workingTreePath(const QString& path) const;
    void showDeleteModifyQuestion(const ConflictEntry& entry);
    /// Clears every region answer and resizes the per-line masks to the loaded
    /// document. Called whenever the file is (re)loaded, so a stale mask can never
    /// be applied to a different region.
    void resetRegionChoices();
    void refreshResult();
    QString renderCards() const;
    QString regionHtml(std::size_t index, const ConflictRegion& region) const;
    QString chipHtml(std::size_t index, int choice, const QString& label) const;
    QString binarySideNotice(const Oid& oid, const char* side) const;
    void applyChoice(int region, int choice);
    /// Sets every region to `choice` — the whole-file "Take ours" buttons, made to
    /// mean the same thing whether or not the file has regions. For a binary file
    /// there are no regions to set, so a choice keeps that whole side instead.
    void applyChoiceToAll(int choice);
    void keepCurrentSide(int choice);
    /// Shows the card browser or the plain editor, and disables the toggle when
    /// there are no regions to show.
    void updateResultMode();

    /// Fills a pane with the text, coloured for the file being resolved, with the
    /// words a word-level diff flagged laid over `emphasisBackground` (one range
    /// list per line).
    void setPaneText(QTextEdit* pane, const QString& text,
                     const std::vector<QVector<QPair<int, int>>>& emphasis = {},
                     const QString& emphasisBackground = QString());
    void setStatus(const QString& text);
    void updateButtonState();
    void scrollToRegion(int index);
    void updateRegionLabel();

    RepoSession* m_session = nullptr;
    QTreeWidget* m_fileList = nullptr;
    QTreeWidget* m_resolvedList = nullptr;
    QStringList m_resolvedPaths;
    QTextEdit* m_ours = nullptr;
    QTextEdit* m_base = nullptr;
    QTextEdit* m_theirs = nullptr;
    QTextBrowser* m_result = nullptr;
    QTextEdit* m_resultEdit = nullptr;
    QLabel* m_header = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_regionLabel = nullptr;
    QPushButton* m_completeButton = nullptr;
    QPushButton* m_abortButton = nullptr;
    QPushButton* m_saveButton = nullptr;
    QPushButton* m_manualButton = nullptr;
    QPushButton* m_rerereButton = nullptr;
    QPushButton* m_takeBothButton = nullptr;
    QPushButton* m_smartButton = nullptr;
    QPushButton* m_stageWorkdirButton = nullptr;
    QPushButton* m_externalButton = nullptr;
    QPushButton* m_deleteButton = nullptr;
    /// The explicit question a delete/modify conflict asks, shown above the panes
    /// instead of leaving the reader to infer it from an empty side.
    QFrame* m_deleteModifyPanel = nullptr;
    QLabel* m_deleteModifyText = nullptr;
    QPushButton* m_keepModifiedButton = nullptr;
    QPushButton* m_keepBaseButton = nullptr;
    QPushButton* m_keepDiskButton = nullptr;
    QPushButton* m_saveDraftButton = nullptr;
    QCheckBox* m_syncScroll = nullptr;
    QLineEdit* m_findField = nullptr;
    QCheckBox* m_findCase = nullptr;
    /// The way out of the page. Not a "resolve" control — the operation stays open
    /// and the reader can come back to it.
    QToolButton* m_close = nullptr;

    std::vector<ConflictEntry> m_conflicts;
    int m_current = -1;

    ConflictDocument m_document;
    std::vector<int> m_choices;
    /// Per-line takes, one entry per region, kept beside `m_choices`: a whole-side
    /// answer uses the choice, a per-line one uses this.
    std::vector<RegionLineTake> m_lineTake;
    int m_currentRegion = -1;
    QString m_path;
    QString m_workdirMarked;
    bool m_manualMode = false;
    /// True when the conflict has no text to merge (a binary file). The panes
    /// show sizes, the editor is disabled, and a side is kept whole.
    bool m_binary = false;
    bool m_deleteModify = false;
    /// rerere state for the file on screen, recomputed by showEntry().
    bool m_rerereRemembered = false;
    bool m_rerereApplied = false;
    /// Recursion guard: one pane's scroll moving the others must not move it back.
    bool m_syncing = false;
};

} // namespace gk
