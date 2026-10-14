#pragma once

#include "core/types.h"

#include <QHash>
#include <QString>
#include <QVector>
#include <QWidget>

#include <string>

class QComboBox;
class QLabel;
class QSplitter;
class QStackedWidget;
class QTextBrowser;
class QTextEdit;
class QToolButton;

namespace gk {

class Skeleton;
class DiffMinimap;
class ElidedLabel;

/// One file's changes, and the file they happened to.
///
/// Two things can be shown: what changed, and what the file says. The second is
/// not decoration — the answer to "what does this change do" is regularly "show me
/// the rest of the function", and opening an editor for that is a detour.
///
/// Content arrives as raw bytes. Git stores bytes and records no encoding, so the
/// decoding happens here, by a codec that is visible and changeable rather than
/// assumed.
class DiffView : public QWidget {
    Q_OBJECT

public:
    /// What a change can be moved between. A commit's diff has no side, and so
    /// offers no staging at all.
    enum class Staging { None, Stage, Unstage };

    struct Document {
        QString path;              ///< for the header; may carry a rename
        FileDiff diff;
        /// The file at the revision being viewed, when it exists there. For a
        /// staged working-tree diff this is the index's version.
        std::string revisionContent;
        bool hasRevision = false;
        std::string workingContent;
        bool hasWorking = false;
        /// What the File View is showing: "HEAD", a short hash, "Index", "Working tree".
        QString contentLabel;
        /// Which way a hunk or line moves. `None` for a commit's diff.
        Staging staging = Staging::None;
        /// True for the unstaged side of a working tree, where a change can also be
        /// thrown away. Discarding destroys content, so the caller confirms it.
        bool offerDiscard = false;
    };

    explicit DiffView(QWidget* parent = nullptr);

    void show(const Document& document);
    /// What is on screen: the document's path (empty when cleared) and its diff.
    const Document& document() const { return m_document; }
    /// Skeleton rows over the text while the next document is being read; `show`
    /// takes them down.
    void setLoading(bool loading);
    bool loading() const;
    void clear();

    /// Re-renders the shown document after a theme or zoom change: the markup
    /// carries the monospace size and the colours, so it has to be rebuilt rather
    /// than merely repainted.
    void refreshTheme();

    /// Chosen codec; empty means "detect it from the bytes".
    QString codec() const { return m_codec; }
    void setCodec(const QString& codec);
    /// Switches between the diff and the file's content. Used by the headless
    /// snapshot, which cannot click a toggle.
    void setFileView(bool on);
    bool splitView() const { return m_split; }
    /// Switches the diff between unified and side-by-side. Used by the headless
    /// snapshot and by tests, which cannot click the toggle.
    void setSplitView(bool on);
    bool blameView() const { return m_blame; }
    void setBlameView(bool on);
    /// Supplies the blame for the file currently shown. The window fetches it, because
    /// the viewer holds no repository.
    void setBlame(const std::vector<BlameLine>& lines);
    /// Soft-wraps long lines instead of scrolling sideways. A toggle, not a mode: it
    /// applies to whichever view is showing.
    bool wrapEnabled() const { return m_wrap; }
    void setWrap(bool on);
    /// Test seam: the number of rendered blocks on a split pane (0 = left, 1 = right).
    /// The two must agree, or a change would not line up with its counterpart.
    int splitPaneBlocks(int pane) const;
    bool minimapVisible() const;
    int minimapMarkCount() const;

    /// Reports a staging request as a click on a hunk's control would. Public so a
    /// test can drive what a mouse cannot be delivered to offscreen.
    void requestHunkStaging(int hunkIndex);
    void requestLineStaging(int hunkIndex, int lineIndex);
    void requestHunkDiscard(int hunkIndex);
    void requestLineDiscard(int hunkIndex, int lineIndex);

    /// Moves to the next or previous change, wrapping at the ends. These are what
    /// the arrows in the toolbar call, and they are public so the headless test can
    /// drive them: a click cannot be delivered to an offscreen window.
    void goToNextChange();
    void goToPreviousChange();
    /// Index of the change the cursor is on, or -1 before the first jump.
    int currentChange() const { return m_currentHunk; }
    int changeCount() const { return m_hunkHeaders.size(); }

signals:
    /// The user closed the diff: the graph should come back.
    void closeRequested();
    /// The user picked a codec. The caller remembers it: a project written in
    /// CP1251 is written in CP1251 in every file.
    void codecChosen(QString codec);
    /// The reader asked to move a hunk, or a set of its lines, between the index
    /// and the working tree. `lines` holds indices into the hunk's lines; empty
    /// means the whole hunk. `toIndex` is true for staging, false for unstaging.
    void changeStagedRequested(QString path, int hunkIndex, QVector<int> lines, bool toIndex);
    /// The reader asked to throw an unstaged change away. `lines` empty means the
    /// whole hunk. Separate from staging because it destroys the change — content
    /// the operation log cannot put back — and the window confirms it.
    void discardRequested(QString path, int hunkIndex, QVector<int> lines);
    /// Blame was switched on for the shown file; the window fetches it and calls
    /// setBlame(), because the viewer holds no repository.
    void blameRequested(QString path);
    void commitActivated(QString commit);
    /// The reader asked to open the shown file in the configured external diff tool.
    /// The window resolves which two revisions the viewer is showing.
    void externalDiffRequested(QString path);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    /// The mode buttons drop their words when the pane is narrower than the bar
    /// with them, rather than holding the window wide.
    void fitToolbar();

    void buildUi();
    void rebuild();
    QString decodeBytes(const std::string& bytes) const;
    QString renderDiff();
    QString renderContent() const;
    /// One side of the side-by-side view as HTML. The two sides are built from the
    /// same aligned rows, so a change reads across and the rows stay in step.
    QString renderSplit(bool left);
    /// The file with a per-line provenance gutter. Empty when no blame was supplied.
    QString renderBlame() const;
    void updateHunkHeaders();
    void gotoChange(int delta);
    void showMessage(const QString& html);
    void showSplitMessage(const QString& html);
    void emitStaging(int hunkIndex, const QVector<int>& lines);
    void emitDiscard(int hunkIndex, const QVector<int>& lines);
    /// The hunk and line a point in the diff lands on, from the map built while
    /// rendering. `line` is -1 for a hunk header or a place outside any hunk.
    QPair<int, int> hunkAt(const QPoint& pos) const;
    QPair<int, int> hunkAtPane(QTextBrowser* pane, const QVector<QPair<int, int>>& map,
                               const QPoint& pos) const;
    void showHunkMenu(QTextBrowser* pane, const QVector<QPair<int, int>>& map, const QPoint& pos);
    /// Mirrors `source`'s vertical scroll onto the other split pane. Guarded so the
    /// two scrollbars do not drive each other forever.
    void syncScrollFrom(QTextBrowser* source);
    void updateMinimap();
    void updateMinimapViewport();

    Document m_document;
    QString m_codec;
    bool m_fileView = false;
    bool m_split = false;
    bool m_blame = false;
    bool m_wrap = false;
    bool m_syncingScroll = false;
    /// Blame by line number (1-based). The window fetches it; the viewer only draws it.
    QHash<int, BlameLine> m_blameByLine;

    ElidedLabel* m_pathLabel = nullptr;
    QWidget* m_bar = nullptr;
    int m_fullBarWidth = 0;
    QComboBox* m_encoding = nullptr;
    QToolButton* m_close = nullptr;
    QToolButton* m_diffMode = nullptr;
    QToolButton* m_splitMode = nullptr;
    QToolButton* m_blameMode = nullptr;
    QToolButton* m_wrapMode = nullptr;
    QToolButton* m_fileMode = nullptr;
    QToolButton* m_external = nullptr;
    QToolButton* m_previous = nullptr;
    QToolButton* m_next = nullptr;
    /// Holds the single browser (unified or file) or the splitter (side by side).
    QStackedWidget* m_pages = nullptr;
    Skeleton* m_skeleton = nullptr;
    QSplitter* m_splitter = nullptr;
    /// The overview beside the viewer; hidden when there is nothing to mark.
    DiffMinimap* m_minimap = nullptr;
    /// A text browser, not a plain text edit: the per-hunk stage buttons are links,
    /// and a read-only browser is what makes them clickable.
    QTextBrowser* m_text = nullptr;
    QTextBrowser* m_textLeft = nullptr;
    QTextBrowser* m_textRight = nullptr;

    /// The rendered text of each hunk's header, used to find the hunk in the
    /// document. Searching for the header is steadier than remembering character
    /// offsets, which HTML rendering moves.
    QVector<QString> m_hunkHeaders;
    int m_currentHunk = -1;
    /// One entry per rendered block: which hunk and line it is. Used to turn a
    /// right-click on a line back into the change it belongs to. The side-by-side
    /// view keeps one per pane.
    QVector<QPair<int, int>> m_lineMap;
    QVector<QPair<int, int>> m_lineMapRight;
};

} // namespace gk
