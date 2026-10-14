#include "ui/diff_view.h"

#include "ui/skeleton.h"

#include "core/diff_rows.h"
#include "ui/diff_minimap.h"
#include "ui/elided_label.h"
#include "ui/syntax_highlight.h"

#include "model/text_encoding.h"
#include "model/word_diff.h"
#include "theme/icons.h"
#include "theme/theme.h"
#include "ui/themed_icons.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace gk {
namespace {

/// More than this and the viewer stops being a viewer: the diff is cut off and
/// says so, rather than taking a second to lay out something nobody will read.
constexpr int kMaxLines = 5000;

QString escaped(const QString& text) {
    return text.toHtmlEscaped();
}

/// The `<pre>` every diff/file/blame view opens with. The size comes from the
/// theme so the reader's zoom reaches the diff too — the markup carries the
/// monospace size, and a fixed one would ignore the preference.
QString preStyle() {
    return QStringLiteral("<pre style=\"font-family:'%2', monospace; font-size:%1px; margin:0; "
                          "white-space:pre\">")
        .arg(theme::monoFontPixelSize())
        .arg(theme::uiFontFamily());
}

/// `top` laid over `bottom` at `alpha`. Used for the emphasis inside a changed
/// line: the row already has the added/removed colour, and the changed words need
/// to be visibly stronger without becoming a second, unrelated colour.
QColor over(const QColor& top, const QColor& bottom, qreal alpha) {
    return QColor(static_cast<int>(top.red() * alpha + bottom.red() * (1.0 - alpha)),
                  static_cast<int>(top.green() * alpha + bottom.green() * (1.0 - alpha)),
                  static_cast<int>(top.blue() * alpha + bottom.blue() * (1.0 - alpha)));
}

QString lineGutter(int oldLine, int newLine) {
    const QString left = oldLine > 0 ? QString::number(oldLine) : QString();
    const QString right = newLine > 0 ? QString::number(newLine) : QString();
    return QStringLiteral("<span style=\"color:%1\">%2 %3</span> ")
        .arg(theme::textDim().name(), left.rightJustified(5), right.rightJustified(5));
}

/// One line of a diff, wrapped in its row colour. The words that changed are
/// already wrapped by the caller, in a stronger shade of the same colour.
QString rowFor(const QString& body, const QColor& background, const QColor& colour,
               const QString& marker) {
    if (background.isValid()) {
        return QStringLiteral("<span style=\"background:%1; color:%2\">%3%4</span>\n")
            .arg(background.name(), colour.name(), marker, body);
    }
    return QStringLiteral("%1%2\n").arg(marker, body);
}

/// A hunk's inline control: a link the browser turns into a signal rather than a
/// navigation. `host` is `hunk` for staging and `discard` for throwing the change away.
QString hunkChip(int hunkIndex, const QString& host, const QString& label) {
    return QStringLiteral(
               " <a href=\"gk://%1/%2\" style=\"color:%3; text-decoration:none\">%4</a>")
        .arg(host)
        .arg(hunkIndex)
        .arg(theme::accent().name(), label);
}

} // namespace

DiffView::DiffView(QWidget* parent) : QWidget(parent) {
    buildUi();
    fitToolbar();
    // A keyboard way to flip the diff, so the side-by-side view is reachable without
    // hunting for the toolbar button.
    auto* split = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+S")), this);
    connect(split, &QShortcut::activated, this, [this] { setSplitView(!m_split); });
}

void DiffView::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bar = new QWidget(this);
    m_bar = bar;
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(6, 4, 6, 4);
    barLayout->setSpacing(6);

    // The file's own name, where the tab would be: the diff is one file's, and the
    // list below the commit can hold hundreds.
    m_pathLabel = new ElidedLabel(bar);
    m_pathLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    barLayout->addWidget(m_pathLabel);

    m_diffMode = new QToolButton(bar);
    m_diffMode->setText(QStringLiteral("Diff View"));
    setThemedIcon(m_diffMode, Icon::FileModified);
    m_diffMode->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_diffMode->setCheckable(true);
    m_diffMode->setChecked(true);
    m_splitMode = new QToolButton(bar);
    m_splitMode->setText(QStringLiteral("Split View"));
    setThemedIcon(m_splitMode, Icon::Split);
    m_splitMode->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_splitMode->setCheckable(true);
    m_splitMode->setToolTip(QStringLiteral("Side by side (Ctrl+Alt+S)"));
    m_fileMode = new QToolButton(bar);
    m_fileMode->setText(QStringLiteral("File View"));
    setThemedIcon(m_fileMode, Icon::FileCode);
    m_fileMode->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_fileMode->setCheckable(true);
    m_blameMode = new QToolButton(bar);
    m_blameMode->setText(QStringLiteral("Blame"));
    setThemedIcon(m_blameMode, Icon::Eye);
    m_blameMode->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_blameMode->setCheckable(true);
    m_blameMode->setToolTip(QStringLiteral("Who last changed each line"));
    auto* modes = new QButtonGroup(bar);
    modes->setExclusive(true);
    modes->addButton(m_diffMode);
    modes->addButton(m_splitMode);
    modes->addButton(m_fileMode);
    modes->addButton(m_blameMode);
    barLayout->addWidget(m_diffMode);
    barLayout->addWidget(m_splitMode);
    barLayout->addWidget(m_fileMode);
    barLayout->addWidget(m_blameMode);

    m_previous = new QToolButton(bar);
    setThemedIcon(m_previous, Icon::ChevronUp);
    m_previous->setToolTip(QStringLiteral("Previous change"));
    m_next = new QToolButton(bar);
    setThemedIcon(m_next, Icon::ChevronDown);
    m_next->setToolTip(QStringLiteral("Next change"));
    barLayout->addWidget(m_previous);
    barLayout->addWidget(m_next);

    // Word wrap is a toggle, not a mode: it applies to whichever view is showing.
    m_wrapMode = new QToolButton(bar);
    m_wrapMode->setText(QStringLiteral("Wrap"));
    setThemedIcon(m_wrapMode, Icon::Wrap);
    m_wrapMode->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_wrapMode->setCheckable(true);
    m_wrapMode->setToolTip(QStringLiteral("Soft-wrap long lines"));
    barLayout->addWidget(m_wrapMode);

    // The external tool is chosen in Preferences and configured in the git config;
    // the viewer only says "open this one, with whatever pair I am showing".
    m_external = new QToolButton(bar);
    m_external->setText(QStringLiteral("External"));
    setThemedIcon(m_external, Icon::Monitor);
    m_external->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_external->setObjectName(QStringLiteral("externalDiff"));
    m_external->setToolTip(QStringLiteral("Open this file in the external diff tool (diff.tool)"));
    barLayout->addWidget(m_external);
    connect(m_external, &QToolButton::clicked, this, [this] {
        if (!m_document.path.isEmpty()) {
            emit externalDiffRequested(m_document.path);
        }
    });

    barLayout->addStretch(1);
    m_encoding = new QComboBox(bar);
    m_encoding->addItem(QStringLiteral("Auto"), QString());
    for (const QString& codec : TextEncoding::codecs()) {
        m_encoding->addItem(codec, codec);
    }
    m_encoding->setToolTip(QStringLiteral("Encoding of the file's bytes"));
    // Compact: it names a codec, and a combo that spans the toolbar looks like a
    // text field waiting for input.
    m_encoding->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_encoding->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_encoding->setMinimumWidth(140);
    barLayout->addWidget(m_encoding);

    // Closing the diff is how the graph comes back, and it sits at the right end
    // where the way out of a pane is looked for.
    m_close = new QToolButton(bar);
    m_close->setObjectName(QStringLiteral("diffClose"));
    setThemedIcon(m_close, Icon::Close);
    m_close->setToolTip(QStringLiteral("Back to the graph (Esc)"));
    barLayout->addWidget(m_close);
    connect(m_close, &QToolButton::clicked, this, &DiffView::closeRequested);

    layout->addWidget(bar);

    auto makeBrowser = [this](const QString& placeholder) {
        auto* browser = new QTextBrowser(this);
        browser->setReadOnly(true);
        browser->setLineWrapMode(QTextEdit::NoWrap);
        browser->setFont(theme::monoFont());
        browser->setPlaceholderText(placeholder);
        // The per-hunk "Stage hunk"/"Unstage hunk" controls are links. A browser would
        // otherwise try to navigate to them and wipe the diff; anchorClicked is what
        // turns a click into a signal instead.
        browser->setOpenLinks(false);
        browser->setOpenExternalLinks(false);
        browser->setContextMenuPolicy(Qt::CustomContextMenu);
        return browser;
    };

    m_pages = new QStackedWidget(this);

    m_skeleton = new Skeleton(m_pages, Skeleton::Shape::List);

    m_text = makeBrowser(QStringLiteral("Select a file to see its changes"));
    m_pages->addWidget(m_text);

    // The side-by-side view: two browsers whose vertical scrollbars are kept in step.
    // They are built from the same aligned rows, so a change sits on one row.
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_textLeft = makeBrowser(QStringLiteral("Select a file to see its changes"));
    m_textRight = makeBrowser(QString());
    m_splitter->addWidget(m_textLeft);
    m_splitter->addWidget(m_textRight);
    m_splitter->setChildrenCollapsible(false);
    m_pages->addWidget(m_splitter);

    {
        auto* centre = new QHBoxLayout();
        centre->setContentsMargins(0, 0, 0, 0);
        centre->setSpacing(0);
        centre->addWidget(m_pages, 1);
        m_minimap = new DiffMinimap(this);
        m_minimap->setVisible(false);
        centre->addWidget(m_minimap);
        layout->addLayout(centre, 1);
    }

    const auto onAnchor = [this](const QUrl& url) {
        if (url.scheme() != QStringLiteral("gk")) {
            return;
        }
        if (url.host() == QStringLiteral("blame")) {
            // The path of a blame link is the commit id, not a hunk index.
            emit commitActivated(url.path().mid(1));
            return;
        }
        bool ok = false;
        const int hunkIndex = url.path().mid(1).toInt(&ok);
        if (!ok) {
            return;
        }
        if (url.host() == QStringLiteral("hunk")) {
            emitStaging(hunkIndex, {});
        } else if (url.host() == QStringLiteral("discard")) {
            emitDiscard(hunkIndex, {});
        }
    };
    connect(m_text, &QTextBrowser::anchorClicked, this, onAnchor);
    connect(m_textLeft, &QTextBrowser::anchorClicked, this, onAnchor);

    connect(m_text, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) { showHunkMenu(m_text, m_lineMap, pos); });
    connect(m_textLeft, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) { showHunkMenu(m_textLeft, m_lineMap, pos); });
    connect(m_textRight, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) { showHunkMenu(m_textRight, m_lineMapRight, pos); });

    connect(m_textLeft->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { syncScrollFrom(m_textLeft); });
    connect(m_textRight->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { syncScrollFrom(m_textRight); });

    // The minimap: scrub to scroll, and follow whichever pane is showing.
    connect(m_minimap, &DiffMinimap::scrubbed, this, [this](qreal fraction) {
        QTextBrowser* active = m_split ? m_textLeft : m_text;
        QScrollBar* bar = active->verticalScrollBar();
        const int total = bar->maximum() + bar->pageStep();
        if (total <= 0) {
            return;
        }
        const int value = static_cast<int>(fraction * total - bar->pageStep() / 2.0);
        bar->setValue(std::clamp(value, 0, bar->maximum()));
    });
    for (QTextBrowser* browser : {m_text, m_textLeft, m_textRight}) {
        connect(browser->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this] { updateMinimapViewport(); });
        connect(browser->verticalScrollBar(), &QScrollBar::rangeChanged, this,
                [this] { updateMinimapViewport(); });
    }

    connect(m_diffMode, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            m_fileView = false;
            m_split = false;
            m_blame = false;
            rebuild();
        }
    });
    connect(m_splitMode, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            m_fileView = false;
            m_split = true;
            m_blame = false;
            rebuild();
        }
    });
    connect(m_fileMode, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            m_fileView = true;
            m_split = false;
            m_blame = false;
            rebuild();
        }
    });
    connect(m_blameMode, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            m_fileView = false;
            m_split = false;
            m_blame = true;
            m_pages->setCurrentWidget(m_text);
            rebuild();
            // The window holds the repository; it answers with the blame.
            emit blameRequested(m_document.path);
        }
    });
    connect(m_wrapMode, &QToolButton::toggled, this, [this](bool on) { setWrap(on); });
    connect(m_previous, &QToolButton::clicked, this, [this] { goToPreviousChange(); });
    connect(m_next, &QToolButton::clicked, this, [this] { goToNextChange(); });
    connect(m_encoding, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_codec = m_encoding->itemData(index).toString();
        emit codecChosen(m_codec);
        rebuild();
    });
}

void DiffView::setCodec(const QString& codec) {
    m_codec = codec;
    const int index = m_encoding->findData(codec);
    if (index >= 0 && index != m_encoding->currentIndex()) {
        const QSignalBlocker blocker(m_encoding);
        m_encoding->setCurrentIndex(index);
    }
}

void DiffView::goToNextChange() {
    gotoChange(1);
}

void DiffView::goToPreviousChange() {
    gotoChange(-1);
}

void DiffView::setFileView(bool on) {
    if (on) {
        m_fileMode->setChecked(true);
    } else {
        m_diffMode->setChecked(true);
    }
}

void DiffView::setSplitView(bool on) {
    if (on) {
        m_splitMode->setChecked(true);
    } else {
        m_diffMode->setChecked(true);
    }
}

void DiffView::setBlameView(bool on) {
    if (on) {
        m_blameMode->setChecked(true);
    } else {
        m_diffMode->setChecked(true);
    }
}

void DiffView::setBlame(const std::vector<BlameLine>& lines) {
    m_blameByLine.clear();
    for (const BlameLine& line : lines) {
        m_blameByLine.insert(line.finalLine, line);
    }
    if (m_blame) {
        rebuild();
    }
}

void DiffView::setWrap(bool on) {
    m_wrap = on;
    if (m_wrapMode && m_wrapMode->isChecked() != on) {
        const QSignalBlocker block(m_wrapMode);
        m_wrapMode->setChecked(on);
    }
    // The widget-level wrap is what makes a soft-wrapped line scrollable; the markup's
    // `white-space` is what lets the browser wrap at all, and both have to agree.
    const QTextEdit::LineWrapMode mode = on ? QTextEdit::WidgetWidth : QTextEdit::NoWrap;
    for (QTextBrowser* browser : {m_text, m_textLeft, m_textRight}) {
        browser->setLineWrapMode(mode);
    }
    rebuild();
}

int DiffView::splitPaneBlocks(int pane) const {
    QTextBrowser* browser = pane == 0 ? m_textLeft : m_textRight;
    return browser ? browser->document()->blockCount() : 0;
}

bool DiffView::minimapVisible() const {
    // `isVisible()` is false while the window has never been shown; `isHidden()` is the
    // question asked here.
    return m_minimap && !m_minimap->isHidden();
}

int DiffView::minimapMarkCount() const {
    return m_minimap ? m_minimap->markCount() : 0;
}

void DiffView::requestHunkStaging(int hunkIndex) {
    emitStaging(hunkIndex, {});
}

void DiffView::requestLineStaging(int hunkIndex, int lineIndex) {
    emitStaging(hunkIndex, {lineIndex});
}

void DiffView::requestHunkDiscard(int hunkIndex) {
    emitDiscard(hunkIndex, {});
}

void DiffView::requestLineDiscard(int hunkIndex, int lineIndex) {
    emitDiscard(hunkIndex, {lineIndex});
}

void DiffView::show(const Document& document) {
    m_skeleton->setActive(false);
    m_document = document;
    m_currentHunk = -1;
    rebuild();
}

void DiffView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    fitToolbar();
}

void DiffView::fitToolbar() {
    const std::array<QToolButton*, 6> worded{m_diffMode, m_splitMode, m_fileMode,
                                            m_blameMode, m_wrapMode, m_external};
    const auto setCompact = [&](bool compact) {
        for (QToolButton* button : worded) {
            button->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly
                                               : Qt::ToolButtonTextBesideIcon);
        }
    };
    if (m_fullBarWidth == 0) {
        // The bar's floor is its icon-only width, so the pane, and the window, can be
        // narrower than the words on the buttons.
        setCompact(true);
        const int compactWidth = m_bar->layout()->minimumSize().width();
        setCompact(false);
        m_fullBarWidth = m_bar->layout()->minimumSize().width();
        m_bar->setMinimumWidth(compactWidth);
    }
    const bool compact = width() < m_fullBarWidth;
    if (compact != (worded.front()->toolButtonStyle() == Qt::ToolButtonIconOnly)) {
        setCompact(compact);
    }
}

void DiffView::clear() {
    m_document = Document{};
    m_hunkHeaders.clear();
    m_currentHunk = -1;
    m_pathLabel->setFullText(QString());
    if (m_split) {
        showSplitMessage(QStringLiteral("Select a file to see its changes"));
    } else {
        showMessage(QStringLiteral("Select a file to see its changes"));
    }
}

void DiffView::refreshTheme() {
    m_fullBarWidth = 0;
    fitToolbar();
    m_pathLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    if (!m_document.path.isEmpty()) {
        rebuild();
    }
}

QString DiffView::decodeBytes(const std::string& bytes) const {
    return TextEncoding::decode(bytes, m_codec);
}

void DiffView::rebuild() {
    if (m_document.path.isEmpty()) {
        m_pathLabel->setFullText(QString());
        showMessage(QStringLiteral("Select a file to see its changes"));
        return;
    }

    // What the bytes say they are, shown on the picker: an encoding the user never
    // chose is still an encoding they need to be able to check.
    const std::string& probe = m_document.hasRevision ? m_document.revisionContent
                                                      : m_document.workingContent;
    const QString detected = TextEncoding::detect(probe, m_document.hasWorking
                                                            ? QString()
                                                            : QStringLiteral("UTF-8"));
    // On Auto the picker shows what the bytes turned out to be rather than the word
    // "Auto": an encoding the reader never chose is still one they need to be able
    // to see, and it is the answer to "why does this file look like that".
    if (m_codec.isEmpty()) {
        const int shown = m_encoding->findData(detected);
        if (shown >= 0 && shown != m_encoding->currentIndex()) {
            const QSignalBlocker blocker(m_encoding);
            m_encoding->setCurrentIndex(shown);
        }
    }
    m_encoding->setToolTip(m_codec.isEmpty() ? QStringLiteral("Detected as %1").arg(detected)
                                             : QStringLiteral("Read as %1").arg(m_codec));

    m_pathLabel->setFullText(m_document.path);

    m_fileMode->setEnabled(m_document.hasRevision || m_document.hasWorking);
    m_splitMode->setEnabled(!m_fileView);
    const bool navigating = !m_fileView && !m_split && !m_document.diff.hunks.empty();
    m_previous->setEnabled(navigating);
    m_next->setEnabled(navigating);

    if (m_fileView) {
        m_pages->setCurrentWidget(m_text);
        updateHunkHeaders();
        m_lineMap.clear(); // the File View has no hunks to stage
        m_lineMapRight.clear();
        m_text->setHtml(renderContent());
        updateMinimap();
        return;
    }

    if (m_blame) {
        // The file with a provenance gutter. The blame itself is the window's to
        // fetch; until it arrives the gutter is blank and the file still reads.
        m_pages->setCurrentWidget(m_text);
        updateHunkHeaders();
        m_lineMap.clear();
        m_lineMapRight.clear();
        const QString html = renderBlame();
        if (html.isEmpty()) {
            showMessage(QStringLiteral("No blame for this file."));
        } else {
            m_text->setHtml(html);
        }
        updateMinimap();
        return;
    }

    updateHunkHeaders();
    if (m_split) {
        m_pages->setCurrentWidget(m_splitter);
        if (m_document.diff.binary) {
            showSplitMessage(QStringLiteral("Binary file — no line diff available."));
            return;
        }
        if (m_document.diff.hunks.empty()) {
            showSplitMessage(QStringLiteral("No changes."));
            return;
        }
        m_textLeft->setHtml(renderSplit(true));
        m_textRight->setHtml(renderSplit(false));
        // A fresh document starts at the top; without this the panes keep the old
        // scroll position of whatever file was shown before.
        m_textLeft->verticalScrollBar()->setValue(0);
        m_textRight->verticalScrollBar()->setValue(0);
        updateMinimap();
        return;
    }

    m_pages->setCurrentWidget(m_text);
    m_lineMapRight.clear();
    m_text->setHtml(renderDiff());
    if (m_document.diff.binary) {
        showMessage(QStringLiteral("Binary file — no line diff available."));
        return;
    }
    if (m_document.diff.hunks.empty()) {
        showMessage(QStringLiteral("No changes."));
        return;
    }
    updateMinimap();
    Q_UNUSED(probe);
}

void DiffView::showMessage(const QString& html) {
    m_lineMap.clear();
    m_lineMapRight.clear();
    m_text->setHtml(QStringLiteral("<p style=\"color:%1\">%2</p>")
                        .arg(theme::textDim().name(), html));
    updateMinimap();
}

void DiffView::showSplitMessage(const QString& html) {
    m_lineMap.clear();
    m_lineMapRight.clear();
    m_textLeft->setHtml(QStringLiteral("<p style=\"color:%1\">%2</p>")
                            .arg(theme::textDim().name(), html));
    m_textRight->clear();
    updateMinimap();
}

void DiffView::updateHunkHeaders() {
    m_hunkHeaders.clear();
    for (const DiffHunk& hunk : m_document.diff.hunks) {
        m_hunkHeaders.push_back(QStringLiteral("@@ -%1,%2 +%3,%4 @@")
                                    .arg(hunk.oldStart)
                                    .arg(hunk.oldLines)
                                    .arg(hunk.newStart)
                                    .arg(hunk.newLines));
    }
}

QString DiffView::renderDiff() {
    QString html = preStyle();
    int emitted = 0;
    m_lineMap.clear();

    const Language language = languageForPath(m_document.path);
    const QString removedEmphasis = over(theme::diffRemovedFg(), theme::diffRemovedBg(), 0.38).name();
    const QString addedEmphasis = over(theme::diffAddedFg(), theme::diffAddedBg(), 0.38).name();

    const bool stageable = m_document.staging != Staging::None;
    const QString stageLabel = m_document.staging == Staging::Stage ? QStringLiteral("Stage hunk")
                                                                   : QStringLiteral("Unstage hunk");

    for (std::size_t hi = 0; hi < m_document.diff.hunks.size(); ++hi) {
        if (emitted >= kMaxLines) {
            break;
        }
        const DiffHunk& hunk = m_document.diff.hunks[hi];
        // A fresh state per hunk: the lines a hunk skips are the lines this renderer
        // never sees, and pretending otherwise colours the rest of the file wrong.
        HighlightState state;
        html += QStringLiteral("<span style=\"background:%1; color:%2\">%3</span>")
                    .arg(over(theme::diffHeader(), theme::surface(), 0.14).name(),
                         theme::diffHeader().name(),
                         escaped(QStringLiteral("@@ -%1,%2 +%3,%4 @@")
                                     .arg(hunk.oldStart)
                                     .arg(hunk.oldLines)
                                     .arg(hunk.newStart)
                                     .arg(hunk.newLines)));
        // A working-tree diff offers the change; a commit's diff has no index side
        // and so shows no control at all.
        if (stageable) {
            html += hunkChip(static_cast<int>(hi), QStringLiteral("hunk"), stageLabel);
            if (m_document.offerDiscard) {
                html += hunkChip(static_cast<int>(hi), QStringLiteral("discard"),
                                 QStringLiteral("Discard hunk"));
            }
        }
        html += QLatin1Char('\n');
        m_lineMap.push_back({static_cast<int>(hi), -1});

        // A block of removed lines and the block of added lines that follows are the
        // same edit seen from both sides, so they are paired line by line and the
        // words that moved are emphasised. Which line pairs with which comes from the
        // same alignment the side-by-side view is built from, so the two modes cannot
        // disagree about what changed.
        const std::vector<DiffLine>& lines = hunk.lines;
        std::vector<int> partner(lines.size(), -1);
        for (const DiffRow& row : alignedRows(hunk)) {
            if (row.left >= 0 && row.right >= 0) {
                partner[static_cast<std::size_t>(row.left)] = row.right;
                partner[static_cast<std::size_t>(row.right)] = row.left;
            }
        }

        std::size_t index = 0;
        while (index < lines.size() && emitted < kMaxLines) {
            if (lines[index].kind == DiffLineKind::Context) {
                const QString text = renderHighlightedLine(decodeBytes(lines[index].text), language, state);
                html += lineGutter(lines[index].oldLine, lines[index].newLine) + text + QLatin1Char('\n');
                m_lineMap.push_back({static_cast<int>(hi), static_cast<int>(index)});
                ++index;
                ++emitted;
                continue;
            }

            const std::size_t removedStart = index;
            while (index < lines.size() && lines[index].kind == DiffLineKind::Removed) {
                ++index;
            }
            const std::size_t addedStart = index;
            while (index < lines.size() && lines[index].kind == DiffLineKind::Added) {
                ++index;
            }
            const std::size_t removedCount = addedStart - removedStart;
            const std::size_t addedCount = index - addedStart;

            for (std::size_t i = 0; i < removedCount; ++i) {
                const std::size_t at = removedStart + i;
                const DiffLine& line = lines[at];
                const QString text = decodeBytes(line.text);
                QString body;
                const int other = partner[at];
                if (other >= 0) {
                    const QString counterpart =
                        decodeBytes(lines[static_cast<std::size_t>(other)].text);
                    const WordDiff words = wordDiff(text, counterpart);
                    // The words that changed are laid over the syntax colours rather
                    // than nested inside them: one wrap per piece, both attributes.
                    body = renderHighlightedLine(text, language, state,
                                                 emphasisRanges(words.before), removedEmphasis);
                } else {
                    body = renderHighlightedLine(text, language, state);
                }
                html += lineGutter(line.oldLine, line.newLine) +
                        rowFor(body, theme::diffRemovedBg(), theme::diffRemovedFg(),
                               QStringLiteral("-"));
                m_lineMap.push_back({static_cast<int>(hi), static_cast<int>(at)});
                ++emitted;
            }
            for (std::size_t i = 0; i < addedCount && emitted < kMaxLines; ++i) {
                const std::size_t at = addedStart + i;
                const DiffLine& line = lines[at];
                const QString text = decodeBytes(line.text);
                QString body;
                const int other = partner[at];
                if (other >= 0) {
                    const QString counterpart =
                        decodeBytes(lines[static_cast<std::size_t>(other)].text);
                    const WordDiff words = wordDiff(counterpart, text);
                    body = renderHighlightedLine(text, language, state, emphasisRanges(words.after),
                                                 addedEmphasis);
                } else {
                    body = renderHighlightedLine(text, language, state);
                }
                html += lineGutter(line.oldLine, line.newLine) +
                        rowFor(body, theme::diffAddedBg(), theme::diffAddedFg(),
                               QStringLiteral("+"));
                m_lineMap.push_back({static_cast<int>(hi), static_cast<int>(at)});
                ++emitted;
            }
        }
    }

    if (emitted >= kMaxLines) {
        html += QStringLiteral("<span style=\"color:%1\">… diff truncated at %2 lines</span>\n")
                    .arg(theme::textDim().name())
                    .arg(kMaxLines);
    }
    if (m_wrap) {
        html.replace(QStringLiteral("white-space:pre\""),
                     QStringLiteral("white-space:pre-wrap\""));
    }
    html += QStringLiteral("</pre>");
    return html;
}

QString DiffView::renderContent() const {
    const std::string& bytes = m_document.hasWorking ? m_document.workingContent
                                                     : m_document.revisionContent;
    if (bytes.empty()) {
        return QStringLiteral("<p style=\"color:%1\">%2 has no content here.</p>")
            .arg(theme::textDim().name(), escaped(m_document.path));
    }

    QString html = preStyle();
    const QStringList lines = decodeBytes(bytes).split(QLatin1Char('\n'));
    // One state for the whole file: a block comment or a triple-quoted string opens
    // on one line and closes on another, and the colour has to carry across.
    const Language language = languageForPath(m_document.path);
    HighlightState state;
    for (int i = 0; i < lines.size(); ++i) {
        if (i >= kMaxLines) {
            html += QStringLiteral("<span style=\"color:%1\">… file truncated at %2 lines</span>\n")
                        .arg(theme::textDim().name())
                        .arg(kMaxLines);
            break;
        }
        html += lineGutter(i + 1, 0) + renderHighlightedLine(lines.at(i), language, state) +
                QLatin1Char('\n');
    }
    if (m_wrap) {
        html.replace(QStringLiteral("white-space:pre\""),
                     QStringLiteral("white-space:pre-wrap\""));
    }
    html += QStringLiteral("</pre>");
    return html;
}

QString DiffView::renderSplit(bool left) {
    QString html = preStyle();
    QVector<QPair<int, int>>& map = left ? m_lineMap : m_lineMapRight;
    map.clear();

    const Language language = languageForPath(m_document.path);
    const QString removedEmphasis = over(theme::diffRemovedFg(), theme::diffRemovedBg(), 0.38).name();
    const QString addedEmphasis = over(theme::diffAddedFg(), theme::diffAddedBg(), 0.38).name();
    const QString filler = QStringLiteral("<span style=\"background:%1\">%2</span>")
                               .arg(theme::surfaceAlt().name(), QStringLiteral("&nbsp;"));

    const bool stageable = left && m_document.staging != Staging::None;
    const QString stageLabel = m_document.staging == Staging::Stage ? QStringLiteral("Stage hunk")
                                                                   : QStringLiteral("Unstage hunk");

    int emitted = 0;
    for (std::size_t hi = 0; hi < m_document.diff.hunks.size(); ++hi) {
        if (emitted >= kMaxLines) {
            break;
        }
        const DiffHunk& hunk = m_document.diff.hunks[hi];
        // The same header on both sides, so the rows below it stay aligned. The stage
        // control belongs on the left pane, where the reader's eye starts.
        html += QStringLiteral("<span style=\"background:%1; color:%2\">%3</span>")
                    .arg(over(theme::diffHeader(), theme::surface(), 0.14).name(),
                         theme::diffHeader().name(),
                         escaped(QStringLiteral("@@ -%1,%2 +%3,%4 @@")
                                     .arg(hunk.oldStart)
                                     .arg(hunk.oldLines)
                                     .arg(hunk.newStart)
                                     .arg(hunk.newLines)));
        if (stageable) {
            html += hunkChip(static_cast<int>(hi), QStringLiteral("hunk"), stageLabel);
            if (m_document.offerDiscard) {
                html += hunkChip(static_cast<int>(hi), QStringLiteral("discard"),
                                 QStringLiteral("Discard hunk"));
            }
        }
        html += QLatin1Char('\n');
        map.push_back({static_cast<int>(hi), -1});

        // A fresh state per hunk: the lines a hunk skips are the lines this renderer
        // never sees.
        HighlightState state;
        for (const DiffRow& row : alignedRows(hunk)) {
            if (emitted >= kMaxLines) {
                break;
            }
            const int index = left ? row.left : row.right;
            if (index < 0) {
                html += lineGutter(0, 0) + filler + QLatin1Char('\n');
                map.push_back({static_cast<int>(hi), -1});
                ++emitted;
                continue;
            }
            const DiffLine& line = hunk.lines[static_cast<std::size_t>(index)];
            if (line.kind == DiffLineKind::Context) {
                const QString text = renderHighlightedLine(decodeBytes(line.text), language, state);
                html += left ? lineGutter(line.oldLine, 0) + text + QLatin1Char('\n')
                             : lineGutter(0, line.newLine) + text + QLatin1Char('\n');
                map.push_back({static_cast<int>(hi), index});
                ++emitted;
                continue;
            }
            // A change: emphasise the words against the other side of the same row, so
            // the two panes underline what actually moved.
            const int counterpart = left ? row.right : row.left;
            QString body;
            if (counterpart >= 0) {
                const QString mine = decodeBytes(line.text);
                const QString other =
                    decodeBytes(hunk.lines[static_cast<std::size_t>(counterpart)].text);
                const WordDiff words = left ? wordDiff(mine, other) : wordDiff(other, mine);
                body = renderHighlightedLine(mine, language, state,
                                             emphasisRanges(left ? words.before : words.after),
                                             left ? removedEmphasis : addedEmphasis);
            } else {
                body = renderHighlightedLine(decodeBytes(line.text), language, state);
            }
            if (left) {
                html += lineGutter(line.oldLine, 0) +
                        rowFor(body, theme::diffRemovedBg(), theme::diffRemovedFg(),
                               QStringLiteral("-"));
            } else {
                html += lineGutter(0, line.newLine) +
                        rowFor(body, theme::diffAddedBg(), theme::diffAddedFg(),
                               QStringLiteral("+"));
            }
            map.push_back({static_cast<int>(hi), index});
            ++emitted;
        }
    }

    if (emitted >= kMaxLines) {
        html += QStringLiteral("<span style=\"color:%1\">… diff truncated at %2 lines</span>\n")
                    .arg(theme::textDim().name())
                    .arg(kMaxLines);
    }
    if (m_wrap) {
        html.replace(QStringLiteral("white-space:pre\""),
                     QStringLiteral("white-space:pre-wrap\""));
    }
    html += QStringLiteral("</pre>");
    return html;
}

QString DiffView::renderBlame() const {
    const std::string& bytes =
        m_document.hasWorking ? m_document.workingContent : m_document.revisionContent;
    if (bytes.empty()) {
        return {};
    }
    const QStringList lines = decodeBytes(bytes).split(QLatin1Char('\n'));
    const Language language = languageForPath(m_document.path);
    HighlightState state;

    QString html = preStyle();
    for (int i = 0; i < lines.size(); ++i) {
        if (i >= kMaxLines) {
            html += QStringLiteral("<span style=\"color:%1\">… file truncated at %2 lines</span>\n")
                        .arg(theme::textDim().name())
                        .arg(kMaxLines);
            break;
        }
        const auto entry = m_blameByLine.constFind(i + 1);
        QString gutter;
        if (entry == m_blameByLine.constEnd()) {
            gutter = QStringLiteral("<span style=\"color:%1\">%2</span> ")
                         .arg(theme::textDim().name(), QString(22, QLatin1Char(' ')));
        } else {
            const BlameLine& blame = entry.value();
            if (blame.uncommitted) {
                gutter = QStringLiteral("<span style=\"color:%1\">%2</span> ")
                             .arg(theme::textDim().name(),
                                  QStringLiteral("Not committed").leftJustified(22));
            } else {
                // The short hash is a link, so a click selects the commit in the graph.
                gutter = QStringLiteral("<a href=\"gk://blame/%1\" style=\"color:%2; "
                                        "text-decoration:none\">%3</a> %4 ")
                             .arg(QString::fromStdString(blame.commit), theme::accent().name(),
                                  QString::fromStdString(blame.commit).left(8),
                                  escaped(QString::fromStdString(blame.author).left(12)));
            }
        }
        html += gutter + renderHighlightedLine(lines.at(i), language, state) + QLatin1Char('\n');
    }
    if (m_wrap) {
        html.replace(QStringLiteral("white-space:pre\""),
                     QStringLiteral("white-space:pre-wrap\""));
    }
    html += QStringLiteral("</pre>");
    return html;
}

void DiffView::gotoChange(int delta) {
    if (m_hunkHeaders.isEmpty() || m_fileView || m_blame) {
        return;
    }
    const int count = m_hunkHeaders.size();
    if (m_currentHunk < 0) {
        m_currentHunk = delta > 0 ? 0 : count - 1;
    } else {
        m_currentHunk = (m_currentHunk + delta + count) % count;
    }

    // Found by its own text rather than by a remembered offset: setting HTML makes
    // the document re-lay out, and offsets counted while building the markup are
    // not the ones the document ends up with.
    QTextBrowser* target = m_split ? m_textLeft : m_text;
    const QString header = m_hunkHeaders.at(m_currentHunk);
    QTextCursor cursor = target->document()->find(header);
    if (cursor.isNull()) {
        return;
    }
    QTextCursor end = target->document()->find(m_hunkHeaders.value(m_currentHunk + 1));
    cursor.setPosition(cursor.selectionStart());
    cursor.setPosition(end.isNull() ? target->document()->characterCount() - 1
                                    : end.selectionStart(),
                       QTextCursor::KeepAnchor);
    target->setTextCursor(cursor);
    target->ensureCursorVisible();
}

QPair<int, int> DiffView::hunkAt(const QPoint& pos) const {
    return hunkAtPane(m_text, m_lineMap, pos);
}

QPair<int, int> DiffView::hunkAtPane(QTextBrowser* pane, const QVector<QPair<int, int>>& map,
                                    const QPoint& pos) const {
    if (!pane) {
        return {-1, -1};
    }
    const int block = pane->cursorForPosition(pos).blockNumber();
    if (block < 0 || block >= map.size()) {
        return {-1, -1};
    }
    return map.at(block);
}

void DiffView::showHunkMenu(QTextBrowser* pane, const QVector<QPair<int, int>>& map,
                            const QPoint& pos) {
    if (m_document.staging == Staging::None || map.isEmpty()) {
        return;
    }
    const QPair<int, int> where = hunkAtPane(pane, map, pos);
    const int hunkIndex = where.first;
    const int lineIndex = where.second;
    if (hunkIndex < 0) {
        return;
    }
    const bool toIndex = m_document.staging == Staging::Stage;
    QMenu menu(this);
    QAction* hunkAction =
        menu.addAction(toIndex ? QStringLiteral("Stage hunk") : QStringLiteral("Unstage hunk"));
    connect(hunkAction, &QAction::triggered, this,
            [this, hunkIndex] { emitStaging(hunkIndex, {}); });
    if (toIndex && m_document.offerDiscard) {
        QAction* discardHunk = menu.addAction(QStringLiteral("Discard hunk"));
        connect(discardHunk, &QAction::triggered, this,
                [this, hunkIndex] { emitDiscard(hunkIndex, {}); });
    }

    if (lineIndex >= 0 && static_cast<std::size_t>(hunkIndex) < m_document.diff.hunks.size()) {
        const DiffHunk& hunk = m_document.diff.hunks[static_cast<std::size_t>(hunkIndex)];
        if (static_cast<std::size_t>(lineIndex) < hunk.lines.size() &&
            hunk.lines[static_cast<std::size_t>(lineIndex)].kind != DiffLineKind::Context) {
            QAction* lineAction = menu.addAction(
                toIndex ? QStringLiteral("Stage line") : QStringLiteral("Unstage line"));
            connect(lineAction, &QAction::triggered, this, [this, hunkIndex, lineIndex] {
                emitStaging(hunkIndex, {lineIndex});
            });
            if (toIndex && m_document.offerDiscard) {
                QAction* discardLine = menu.addAction(QStringLiteral("Discard line"));
                connect(discardLine, &QAction::triggered, this, [this, hunkIndex, lineIndex] {
                    emitDiscard(hunkIndex, {lineIndex});
                });
            }
        }
    }
    menu.exec(pane->mapToGlobal(pos));
}

void DiffView::syncScrollFrom(QTextBrowser* source) {
    if (m_syncingScroll || !m_split) {
        return;
    }
    QTextBrowser* other = source == m_textLeft ? m_textRight : m_textLeft;
    m_syncingScroll = true;
    other->verticalScrollBar()->setValue(source->verticalScrollBar()->value());
    m_syncingScroll = false;
}

void DiffView::updateMinimap() {
    QVector<bool> marks;
    // Only a unified/split diff has per-line changes to mark; a file or a blame view
    // has none, and the minimap hides rather than show an empty bar.
    const bool isDiff = !m_fileView && !m_blame && !m_lineMap.isEmpty();
    if (isDiff) {
        marks.reserve(m_lineMap.size());
        for (const QPair<int, int>& entry : m_lineMap) {
            const int hunkIndex = entry.first;
            const int lineIndex = entry.second;
            bool changed = false;
            if (hunkIndex >= 0 &&
                static_cast<std::size_t>(hunkIndex) < m_document.diff.hunks.size() &&
                lineIndex >= 0) {
                const DiffHunk& hunk = m_document.diff.hunks[static_cast<std::size_t>(hunkIndex)];
                if (static_cast<std::size_t>(lineIndex) < hunk.lines.size()) {
                    changed = hunk.lines[static_cast<std::size_t>(lineIndex)].kind !=
                              DiffLineKind::Context;
                }
            }
            marks.push_back(changed);
        }
    }
    m_minimap->setMarks(marks);
    m_minimap->setVisible(!marks.isEmpty());
    updateMinimapViewport();
}

void DiffView::updateMinimapViewport() {
    QTextBrowser* active = m_split ? m_textLeft : m_text;
    const QScrollBar* bar = active->verticalScrollBar();
    const int total = bar->maximum() + bar->pageStep();
    if (total <= 0) {
        m_minimap->setViewport(0.0, 1.0);
        return;
    }
    m_minimap->setViewport(static_cast<qreal>(bar->value()) / total,
                           static_cast<qreal>(bar->pageStep()) / total);
}

void DiffView::emitStaging(int hunkIndex, const QVector<int>& lines) {
    if (m_document.staging == Staging::None || m_document.path.isEmpty()) {
        return;
    }
    if (hunkIndex < 0 || static_cast<std::size_t>(hunkIndex) >= m_document.diff.hunks.size()) {
        return;
    }
    emit changeStagedRequested(m_document.path, hunkIndex, lines,
                               m_document.staging == Staging::Stage);
}

void DiffView::emitDiscard(int hunkIndex, const QVector<int>& lines) {
    if (!m_document.offerDiscard || m_document.path.isEmpty()) {
        return;
    }
    if (hunkIndex < 0 || static_cast<std::size_t>(hunkIndex) >= m_document.diff.hunks.size()) {
        return;
    }
    emit discardRequested(m_document.path, hunkIndex, lines);
}

void DiffView::setLoading(bool loading) {
    m_skeleton->setActive(loading);
}

bool DiffView::loading() const {
    return m_skeleton->isActive();
}

} // namespace gk
