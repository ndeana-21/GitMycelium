#pragma once

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>

namespace gk::theme {

enum class ThemeMode {
    Dark,
    Light,
};

/// The active mode, and the switch that changes it. Switching swaps every token
/// below and bumps `generation()`; callers that cached a pixmap or a stylesheet
/// must re-read rather than keep what they built under the old mode.
ThemeMode mode();
void setMode(ThemeMode newMode);

/// Zoom as a percentage of the base metrics. It scales the application font and
/// every geometry token together, so a hand-painted chip and a styled button
/// still agree about height after the reader zooms.
int zoomPercent();
void setZoomPercent(int percent);

/// Bumped whenever the mode or the zoom changes. A pixmap cache keyed by it can
/// tell a stale render from a current one.
int generation();

int baseFontPixelSize();
int monoFontPixelSize();
/// Scales one of the window's own fixed metrics (an avatar, a glyph side) by the
/// zoom, so a hand-sized element grows with the tokens around it. The window
/// should not keep a second copy of the scale factor.
int scaledPixels(int base);

/// The one font family the whole window draws in: a monospace face, picked from
/// the installed coding fonts, falling back to the system's fixed font. The look
/// follows the opencode terminal UI, where every surface is a grid of characters.
QString uiFontFamily();
QFont monoFont();
/// The smaller face of a branch label: painting, hit-testing and the drag chip
/// all measure with it, so the drawn pill and the grabbed one agree.
QFont labelFont();

/// opencode-flavoured palette, dark by default and light on request.
///
/// `background` is the canvas (chrome and graph), `panel` the raised side panels
/// and dialogs, `surfaceAlt` the element step above a panel (inputs, buttons).

QColor background();
QColor panel();
QColor surface();
QColor surfaceAlt();
QColor border();
QColor borderStrong();
QColor text();
QColor textDim();
QColor accent();
QColor accentAlt();
QColor selection();
QColor warning();
QColor modified();
QColor success();
QColor danger();
QColor onAccent();

/// A row, button or menu entry under the pointer, a held control, and the
/// keyboard-focus outline. State is drawn as a tint over the panel colour rather
/// than as a border, which is what makes a flat UI read as calm instead of as a
/// grid of boxes.
QColor hover();
QColor pressed();
QColor focusRing();

/// Geometry tokens shared by the stylesheet and by the code that paints by hand,
/// so a row, a chip and a button agree about the corner radius and the control
/// height no matter which layer drew them. All of them follow `zoomPercent()`.
int radius();
int radiusSmall();
int controlHeight();
int iconSize();
int rowHeight();

/// Syntax colours, tuned for the active surface. Kept in the theme so the diff
/// view, the conflict panes and the file view cannot drift apart.
QColor syntaxKeyword();
QColor syntaxType();
QColor syntaxFunction();
QColor syntaxString();
QColor syntaxNumber();
QColor syntaxComment();
QColor syntaxProperty();     ///< keys in JSON, YAML and TOML
QColor syntaxDirective();    ///< preprocessor and shebang lines

QColor diffAddedBg();
QColor diffAddedFg();
QColor diffRemovedBg();
QColor diffRemovedFg();
QColor diffHeader();

/// Background laid under the words a word-level diff flagged as changed. One token
/// for the merge editor's panes and cards, kept clear of every syntax colour so a
/// changed identifier does not read as a keyword.
QColor changedWordBg();

QColor laneColor(int lane);
int laneColorCount();

/// Colour for a branch, by index into the branch palette.
///
/// Branch colour is what makes a graph readable: the line leaving a branch and
/// the label carrying its name must be the same colour. The palette is shared
/// with the lanes on purpose — see CommitModel, which owns the assignment.
QColor branchColor(int index);
int branchColorCount();

/// Text and border tints for a branch label, given its colour. A label is a
/// translucent fill with a solid border and coloured text.
QColor branchLabelFill(const QColor& branch);
QColor branchLabelBorder(const QColor& branch);
QColor branchLabelText(const QColor& branch);

/// The fill of an author's avatar when there is no picture: a hue picked from the
/// identity (`email`, or the name when there is none), the same on every run and in
/// every view. Saturation and lightness are the mode's, so every author reads on the
/// background; the hue is spread over the whole circle rather than the branch palette,
/// so an avatar is not mistaken for a branch.
QColor authorColor(const QString& identity);
QColor textOn(const QColor& fill);

QPalette palette();

QString styleSheet();

} // namespace gk::theme
