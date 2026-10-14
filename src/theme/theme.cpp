#include "theme/theme.h"

#include <QFontDatabase>
#include <QStringList>
#include <QPalette>

#include <algorithm>
#include <array>
#include <cstdint>

namespace gk::theme {
namespace {

QColor hex(const char* value) {
    return QColor(QString::fromLatin1(value));
}

/// Every colour the application reads. Two of these exist — one per mode — and
/// the accessors below never touch anything else, so a mode switch is a swap of
/// this struct rather than a repaint of constants buried in call sites.
struct Tokens {
    QColor background;
    QColor panel;
    QColor surface;
    QColor surfaceAlt;
    QColor border;
    QColor borderStrong;
    QColor text;
    QColor textDim;
    QColor accent;
    QColor accentAlt;
    QColor selection;
    QColor warning;
    QColor modified;
    QColor success;
    QColor danger;
    QColor onAccent;
    QColor hover;
    QColor pressed;
    QColor syntaxKeyword;
    QColor syntaxType;
    QColor syntaxFunction;
    QColor syntaxString;
    QColor syntaxNumber;
    QColor syntaxComment;
    QColor syntaxProperty;
    QColor syntaxDirective;
    QColor diffAddedBg;
    QColor diffAddedFg;
    QColor diffRemovedBg;
    QColor diffRemovedFg;
    QColor diffHeader;
    QColor changedWordBg;
    std::array<QColor, 12> branchColors;
    /// Whether `branchLabelText` lifts the branch colour towards white (dark
    /// surface) or sinks it towards black (light surface). The label has to stay
    /// readable on the row behind it, and "readable" points the other way.
    bool liftLabelText;
};

/// The dark set follows the opencode terminal theme: a near-black canvas, panels
/// raised by one step of grey rather than by borders, and one warm accent (peach)
/// for "this is where you are" and for primary controls. The branch palette leads
/// with blue, not the accent, so a branch colour is never mistaken for a control.
///
/// Twelve slots is not arbitrary — every visible branch should be able to get
/// its own colour, and a graph whose branches share a colour is unreadable.
/// Past twelve we wrap, which is unavoidable.
Tokens darkTokens() {
    Tokens t;
    t.background = hex("#0a0a0a");
    t.panel = hex("#141414");
    t.surface = hex("#0a0a0a");
    t.surfaceAlt = hex("#1e1e1e");
    t.border = hex("#262626");
    t.borderStrong = hex("#3c3c3c");
    t.text = hex("#eeeeee");
    t.textDim = hex("#808080");
    t.accent = hex("#fab283");
    t.accentAlt = hex("#5c9cf5");
    t.selection = hex("#2a221c");
    t.warning = hex("#e5c07b");
    t.modified = hex("#f5a742");
    t.success = hex("#7fd88f");
    t.danger = hex("#e06c75");
    t.onAccent = hex("#0a0a0a");
    t.hover = hex("#1a1a1a");
    t.pressed = hex("#262626");
    t.syntaxKeyword = hex("#9d7cd8");
    t.syntaxType = hex("#e5c07b");
    t.syntaxFunction = hex("#fab283");
    t.syntaxString = hex("#7fd88f");
    t.syntaxNumber = hex("#f5a742");
    t.syntaxComment = hex("#6a6a6a");
    t.syntaxProperty = hex("#5c9cf5");
    t.syntaxDirective = hex("#56b6c2");
    t.diffAddedBg = hex("#1b2b25");
    t.diffAddedFg = hex("#4fd6be");
    t.diffRemovedBg = hex("#2f1d22");
    t.diffRemovedFg = hex("#e26a75");
    t.diffHeader = hex("#9d7cd8");
    // A muted violet, kept off the red/green of the diff: a conflict shows both
    // sides at once, and this says only "these words differ", not "this is wrong".
    t.changedWordBg = hex("#2e2640");
    t.branchColors = {
        hex("#5c9cf5"),
        hex("#fab283"),
        hex("#9d7cd8"),
        hex("#7fd88f"),
        hex("#56b6c2"),
        hex("#e06c75"),
        hex("#e5c07b"),
        hex("#4fd6be"),
        hex("#d17ce8"),
        hex("#f5a742"),
        hex("#8fb8ff"),
        hex("#c3e88d"),
    };
    t.liftLabelText = true;
    return t;
}

/// The opencode light set, the same twelve hues darkened for a white surface. A
/// lane that reads well on near-black is washed out on white, and the graph is
/// only readable if the node and the label still carry the same, visible colour.
Tokens lightTokens() {
    Tokens t;
    t.background = hex("#ffffff");
    t.panel = hex("#fafafa");
    t.surface = hex("#ffffff");
    t.surfaceAlt = hex("#f0f0f0");
    t.border = hex("#e2e2e2");
    t.borderStrong = hex("#c4c4c4");
    t.text = hex("#1a1a1a");
    t.textDim = hex("#7a7a7a");
    t.accent = hex("#c2621f");
    t.accentAlt = hex("#3b7dd8");
    t.selection = hex("#f6e7dc");
    t.warning = hex("#a87a00");
    t.modified = hex("#c2621f");
    t.success = hex("#2f8a4a");
    t.danger = hex("#c9363c");
    t.onAccent = hex("#ffffff");
    t.hover = hex("#f2f2f2");
    t.pressed = hex("#e6e6e6");
    t.syntaxKeyword = hex("#7b5bb6");
    t.syntaxType = hex("#9a6a00");
    t.syntaxFunction = hex("#c2621f");
    t.syntaxString = hex("#2f7d32");
    t.syntaxNumber = hex("#b45309");
    t.syntaxComment = hex("#8a8a8a");
    t.syntaxProperty = hex("#3b7dd8");
    t.syntaxDirective = hex("#0f7f8c");
    t.diffAddedBg = hex("#e3f4e7");
    t.diffAddedFg = hex("#1c6b33");
    t.diffRemovedBg = hex("#fbe6e7");
    t.diffRemovedFg = hex("#a1252f");
    t.diffHeader = hex("#7b5bb6");
    t.changedWordBg = hex("#e4ddf3");
    t.branchColors = {
        hex("#2f6fd0"), hex("#c2621f"), hex("#6d4bd0"), hex("#1f8a3b"),
        hex("#0e8a9c"), hex("#c23b57"), hex("#9a7b00"), hex("#0d8a7c"),
        hex("#b23bb0"), hex("#b06a00"), hex("#1f7fb0"), hex("#5f8a00"),
    };
    t.liftLabelText = false;
    return t;
}

ThemeMode g_mode = ThemeMode::Dark;
int g_zoomPercent = 100;
int g_generation = 1;

const Tokens& dark() {
    static const Tokens tokens = darkTokens();
    return tokens;
}

const Tokens& light() {
    static const Tokens tokens = lightTokens();
    return tokens;
}

const Tokens& active() {
    return g_mode == ThemeMode::Light ? light() : dark();
}

int scaled(int base) {
    return std::max(1, static_cast<int>(base * g_zoomPercent / 100.0 + 0.5));
}

} // namespace

ThemeMode mode() {
    return g_mode;
}

void setMode(ThemeMode newMode) {
    if (g_mode == newMode) {
        return;
    }
    g_mode = newMode;
    ++g_generation;
}

int zoomPercent() {
    return g_zoomPercent;
}

void setZoomPercent(int percent) {
    const int clamped = std::clamp(percent, 50, 250);
    if (g_zoomPercent == clamped) {
        return;
    }
    g_zoomPercent = clamped;
    ++g_generation;
}

int generation() {
    return g_generation;
}

int baseFontPixelSize() {
    return std::max(8, static_cast<int>(13 * g_zoomPercent / 100.0 + 0.5));
}

int monoFontPixelSize() {
    return std::max(8, static_cast<int>(12 * g_zoomPercent / 100.0 + 0.5));
}

int scaledPixels(int base) {
    return scaled(base);
}

QColor background() { return active().background; }
QColor panel() { return active().panel; }
QColor surface() { return active().surface; }
QColor surfaceAlt() { return active().surfaceAlt; }
QColor border() { return active().border; }
QColor borderStrong() { return active().borderStrong; }
QColor text() { return active().text; }
QColor textDim() { return active().textDim; }
QColor accent() { return active().accent; }
QColor accentAlt() { return active().accentAlt; }
QColor selection() { return active().selection; }
QColor warning() { return active().warning; }
QColor modified() { return active().modified; }
QColor success() { return active().success; }
QColor danger() { return active().danger; }
QColor onAccent() { return active().onAccent; }
QColor syntaxKeyword() { return active().syntaxKeyword; }
QColor syntaxType() { return active().syntaxType; }
QColor syntaxFunction() { return active().syntaxFunction; }
QColor syntaxString() { return active().syntaxString; }
QColor syntaxNumber() { return active().syntaxNumber; }
QColor syntaxComment() { return active().syntaxComment; }
QColor syntaxProperty() { return active().syntaxProperty; }
QColor syntaxDirective() { return active().syntaxDirective; }

QColor hover() { return active().hover; }
QColor pressed() { return active().pressed; }
QColor focusRing() {
    QColor ring = accent();
    ring.setAlpha(150);
    return ring;
}

int radius() { return scaled(4); }
int radiusSmall() { return scaled(2); }
int controlHeight() { return scaled(28); }
int iconSize() { return scaled(18); }
int rowHeight() { return scaled(30); }

QColor diffAddedBg() { return active().diffAddedBg; }
QColor diffAddedFg() { return active().diffAddedFg; }
QColor diffRemovedBg() { return active().diffRemovedBg; }
QColor diffRemovedFg() { return active().diffRemovedFg; }
QColor diffHeader() { return active().diffHeader; }
QColor changedWordBg() { return active().changedWordBg; }

QColor laneColor(int lane) {
    return branchColor(lane);
}
int laneColorCount() { return static_cast<int>(active().branchColors.size()); }

QColor branchColor(int index) {
    const auto& colors = active().branchColors;
    if (colors.empty()) {
        return accent();
    }
    if (index < 0) {
        index = -index;
    }
    return colors[static_cast<std::size_t>(index) % colors.size()];
}

int branchColorCount() { return static_cast<int>(active().branchColors.size()); }

QColor branchLabelFill(const QColor& branch) {
    QColor fill = branch;
    fill.setAlpha(38);
    return fill;
}

QColor branchLabelBorder(const QColor& branch) {
    QColor border = branch;
    border.setAlpha(170);
    return border;
}

QColor branchLabelText(const QColor& branch) {
    return active().liftLabelText ? branch.lighter(118) : branch.darker(140);
}

QColor authorColor(const QString& identity) {
    // FNV-1a over the bytes, not qHash: qHash may be seeded per process, and an
    // author whose colour changes between runs is not recognisable by it.
    std::uint32_t hash = 2166136261u;
    for (const char byte : identity.trimmed().toLower().toUtf8()) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 16777619u;
    }
    const int hue = static_cast<int>(hash % 360u);
    return active().liftLabelText ? QColor::fromHsl(hue, 150, 120)
                                  : QColor::fromHsl(hue, 160, 105);
}

QColor textOn(const QColor& fill) {
    const double luminance =
        0.2126 * fill.redF() + 0.7152 * fill.greenF() + 0.0722 * fill.blueF();
    return luminance > 0.55 ? QColor(Qt::black) : QColor(Qt::white);
}

QPalette palette() {
    QPalette p;
    p.setColor(QPalette::Window, background());
    p.setColor(QPalette::WindowText, text());
    p.setColor(QPalette::Base, surface());
    p.setColor(QPalette::AlternateBase, surfaceAlt());
    p.setColor(QPalette::Text, text());
    p.setColor(QPalette::Button, surfaceAlt());
    p.setColor(QPalette::ButtonText, text());
    p.setColor(QPalette::BrightText, danger());
    p.setColor(QPalette::Highlight, selection());
    p.setColor(QPalette::HighlightedText, hex("#ffffff"));
    p.setColor(QPalette::ToolTipBase, surfaceAlt());
    p.setColor(QPalette::ToolTipText, text());
    p.setColor(QPalette::PlaceholderText, textDim());
    p.setColor(QPalette::Link, accent());
    p.setColor(QPalette::Disabled, QPalette::Text, textDim());
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDim());
    p.setColor(QPalette::Disabled, QPalette::WindowText, textDim());
    return p;
}

QString uiFontFamily() {
    static const QString family = [] {
        const QStringList installed = QFontDatabase::families();
        for (const char* candidate :
             {"JetBrains Mono", "JetBrainsMono Nerd Font", "Iosevka", "Cascadia Code",
              "Cascadia Mono", "Fira Code", "Hack", "MesloLGS Nerd Font", "DejaVu Sans Mono",
              "Consolas"}) {
            const QString name = QString::fromLatin1(candidate);
            if (installed.contains(name, Qt::CaseInsensitive)) {
                return name;
            }
        }
        return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    }();
    return family;
}

QFont monoFont() {
    QFont font(uiFontFamily());
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(monoFontPixelSize());
    return font;
}

QFont labelFont() {
    QFont font(uiFontFamily());
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(std::max(8, baseFontPixelSize() - 2));
    return font;
}

QString styleSheet() {
    // A disabled filled button is the same colour, see-through: it still says what it
    // would do, and plainly cannot do it yet.
    const auto faded = [](QColor colour) {
        colour.setAlphaF(0.4);
        return colour.name(QColor::HexArgb);
    };
    const auto faint = [](QColor colour) {
        colour.setAlphaF(0.75);
        return colour.name(QColor::HexArgb);
    };
    const bool darkMode = g_mode == ThemeMode::Dark;
    const QColor elementHi = darkMode ? surfaceAlt().lighter(125) : surfaceAlt().darker(106);

    return QStringLiteral(R"(
QWidget { color: %(text)s; }
QMainWindow, QDialog { background: %(bg)s; }
QDialog, QMessageBox { background: %(panel)s; }

/* --- menu bar and menus: a dialog in opencode is a raised panel whose current
       entry is a solid accent bar --- */
QMenuBar { background: %(bg)s; border: none; padding: 2px 4px; }
QMenuBar::item { padding: 4px 10px; background: transparent; color: %(dim)s; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: %(element)s; color: %(text)s; }

QMenu { background: %(panel)s; border: 1px solid %(borderStrong)s; padding: 4px 0px; }
QMenu::item { padding: 5px 28px 5px 14px; background: transparent; }
QMenu::item:selected { background: %(accent)s; color: %(onAccent)s; }
QMenu::item:disabled { color: %(dim)s; }
QMenu::item:disabled:selected { background: %(element)s; color: %(dim)s; }
QMenu::separator { height: 1px; background: %(border)s; margin: 4px 0px; }
QMenu::icon { padding-left: 8px; }
QMenu::indicator { width: 12px; height: 12px; }

/* --- toolbars --- */
QToolBar { background: %(bg)s; border: none; spacing: 2px; padding: 4px 8px; }
QToolBar#tabsToolBar { padding: 0px 4px; border-bottom: 1px solid %(border)s; }
QToolBar::separator { background: %(border)s; width: 1px; margin: 6px 6px; }
QToolBar QToolButton { background: transparent; border: none;
                       border-radius: %(radiusSmall)px; padding: 5px; }
QToolBar QToolButton[variant="menu"] { min-width: %(controlHeight)px;
                       padding-left: 8px; padding-right: 14px; }
QToolBar QToolButton:hover:enabled { background: %(element)s; }
QToolBar QToolButton:pressed { background: %(elementHi)s; }
QToolBar QToolButton:checked { background: %(element)s; color: %(accent)s; }
QToolBar QToolButton:disabled { color: %(dim)s; }
QToolBar QComboBox { min-height: 22px; padding: 2px 8px; }

/* --- plain tool buttons: panel bars, viewer controls --- */
QToolButton { background: transparent; border: none;
              border-radius: %(radiusSmall)px; padding: 4px 6px; }
QToolButton:hover:enabled { background: %(element)s; }
QToolButton:pressed { background: %(elementHi)s; }
QToolButton:checked { background: %(element)s; color: %(accent)s; }
QToolButton:disabled { color: %(dim)s; }
QToolButton::menu-indicator { image: none; width: 0px; }

/* --- buttons: flat blocks, the primary one in the accent --- */
QPushButton { background: %(element)s; color: %(text)s; border: none;
              border-radius: %(radiusSmall)px; padding: 6px 14px; min-height: 22px; }
QPushButton:hover:enabled { background: %(elementHi)s; }
QPushButton:pressed { background: %(pressed)s; }
QPushButton:focus { outline: none; }
QPushButton:default { background: %(accent)s; color: %(onAccent)s; font-weight: 600; }
QPushButton:default:hover:enabled { background: %(accentHi)s; }
QPushButton:disabled { color: %(dim)s; background: %(panel)s; }
QPushButton[variant="primary"] { background: %(accent)s; color: %(onAccent)s;
                                 font-weight: 600; }
QPushButton[variant="primary"]:hover:enabled { background: %(accentHi)s; }
QPushButton[variant="primary"]:pressed { background: %(accentDim)s; }
QPushButton[variant="primary"]:disabled { background: %(element)s; color: %(dim)s; }
QPushButton[variant="danger"] { color: %(danger)s; background: transparent;
                                border: 1px solid %(danger)s; }
QPushButton[variant="danger"]:hover:enabled { background: %(danger)s; color: %(onAccent)s; }
QPushButton[variant="success"] { background: %(success)s; color: %(onAccent)s;
                                 font-weight: 600; }
QPushButton[variant="success"]:hover:enabled { background: %(successHi)s; }
QPushButton[variant="success"]:disabled { background: %(successFaded)s; color: %(onAccentFaded)s; }
QPushButton[variant="dangerSolid"] { background: %(danger)s; color: %(onAccent)s;
                                     font-weight: 600; }
QPushButton[variant="dangerSolid"]:hover:enabled { background: %(dangerHi)s; }
QPushButton[variant="dangerSolid"]:disabled { background: %(dangerFaded)s;
                                              color: %(onAccentFaded)s; }
QPushButton[buttonSize="small"] { padding: 2px 10px; min-height: 16px; }
QPushButton[buttonSize="large"] { padding: 8px 14px; min-height: %(controlHeight)px; }

/* --- inputs: an element block; focus is an accent bar on the left, the way the
       opencode prompt marks where the keyboard is --- */
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateTimeEdit {
    background: %(element)s; border: none; border-left: 2px solid %(element)s;
    border-radius: 0px; padding: 4px 8px; min-height: 22px;
    selection-background-color: %(accent)s; selection-color: %(onAccent)s;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QDateTimeEdit:focus {
    border-left-color: %(accent)s; background: %(elementHi)s;
}
QLineEdit:disabled, QComboBox:disabled { color: %(dim)s; background: %(panel)s;
    border-left-color: %(panel)s; }
QLineEdit[filterError="true"] { border-left-color: %(danger)s; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox::down-arrow { image: none; width: 0px; }
QComboBox QAbstractItemView { background: %(panel)s; border: 1px solid %(borderStrong)s;
                              padding: 2px; outline: none;
                              selection-background-color: %(accent)s;
                              selection-color: %(onAccent)s; }

QTextEdit, QPlainTextEdit { background: %(bg)s; border: none;
                            border-left: 2px solid %(border)s;
                            selection-background-color: %(accent)s;
                            selection-color: %(onAccent)s; }
QTextEdit:focus, QPlainTextEdit:focus { border-left-color: %(accent)s; }
QFrame[role="messageBox"] { background: %(element)s; border: none;
                            border-left: 2px solid %(accent)s; }
QFrame[role="messageBox"] QLineEdit, QFrame[role="messageBox"] QPlainTextEdit {
    background: transparent; border: none; border-radius: 0px; }
QFrame[role="messageBox"] QLineEdit { border-bottom: 1px solid %(borderStrong)s; }
QToolButton[role="faint"] { color: %(dim)s; }
QToolButton[role="sectionToggle"], QToolButton[role="sectionToggle"]:checked {
    background: transparent; border: none; font-weight: 600; text-align: left;
    color: %(dim)s; }
QToolButton[role="sectionToggle"]:hover { background: %(element)s; color: %(text)s; }
QToolButton[role="faint"]:hover { color: %(text)s; }

/* --- item views: the current row is a tint with the text kept, never a
       saturated block over the whole graph --- */
QTreeView, QTreeWidget, QListView, QTableView {
    background: %(surface)s; alternate-background-color: %(panel)s;
    border: none; outline: none; selection-background-color: %(selection)s;
    selection-color: %(text)s;
}
QTreeView::item, QTreeWidget::item, QListView::item { padding: 3px 6px; border: none; }
QTreeView::item:hover, QTreeWidget::item:hover, QListView::item:hover {
    background: %(hover)s; }
QTreeView::item:selected, QTreeWidget::item:selected, QListView::item:selected {
    background: %(selection)s; color: %(text)s;
}
QTreeView::branch:selected { background: %(selection)s; }

/* single-column lists mark the current row with the accent bar */
gk--RefTreeView::item:selected, QListView::item:selected,
QTreeView#commitFiles::item:selected, QTreeView#stagedFiles::item:selected,
QTreeView#unstagedFiles::item:selected {
    color: %(accent)s; }

/* the command palette and the recent list behave like opencode's dialogs */
gk--CommandPalette QTreeWidget, gk--CommandPalette { background: %(panel)s; }
gk--CommandPalette QTreeWidget::item { padding: 5px 10px; }
gk--CommandPalette QTreeWidget::item:selected { background: %(accent)s; color: %(onAccent)s; }
gk--CommandPalette QLineEdit { padding: 8px 10px; }

/* side panels sit one step above the canvas */
QWidget#refRail, QWidget#refRail QTreeView, gk--CommitDetailsPanel,
gk--CommitDetailsPanel QTreeView, gk--CommitDetailsPanel QScrollArea > QWidget > QWidget,
gk--WorkingTreePanel, gk--WorkingTreePanel QTreeView, gk--WorkingTreePanel QListView,
gk--WorkingTreePanel QScrollArea > QWidget > QWidget {
    background: %(panel)s; }

QHeaderView { background: %(surface)s; border: none; }
QHeaderView::section { background: %(surface)s; color: %(dim)s; padding: 5px 8px;
    border: none; border-bottom: 1px solid %(border)s; font-weight: 400; }
QHeaderView::section:hover { color: %(text)s; }
QWidget#refRail QHeaderView::section, gk--CommitDetailsPanel QHeaderView::section,
gk--WorkingTreePanel QHeaderView::section { background: %(panel)s; }

/* --- splitters: a hairline of separation, accent under the pointer --- */
QSplitter::handle { background: %(border)s; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QSplitter::handle:hover { background: %(accent)s; }
QMainWindow::separator { background: %(border)s; width: 1px; height: 1px; }
QDockWidget { titlebar-close-icon: none; }
QDockWidget::title { background: %(panel)s; padding: 4px 8px; color: %(dim)s; }

/* --- group boxes, checkboxes, tabs --- */
QGroupBox { border: none; border-top: 1px solid %(border)s; margin-top: 14px;
    padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 0px; padding: 0 6px 0 0;
    color: %(accent)s; }

QCheckBox, QRadioButton { spacing: 8px; }
QCheckBox::indicator, QRadioButton::indicator { width: 12px; height: 12px;
    border: 1px solid %(borderStrong)s; border-radius: 0px; background: %(element)s; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: %(accent)s; }
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: %(accent)s; border-color: %(accent)s; }
QCheckBox:disabled, QRadioButton:disabled { color: %(dim)s; }
QRadioButton::indicator { border-radius: 7px; }

QTabWidget::pane { border: none; border-top: 1px solid %(border)s; }
QTabBar { background: transparent; }
QTabBar::tab { background: transparent; color: %(dim)s; padding: 5px 12px;
    border: none; border-top: 2px solid transparent; }
QTabBar::tab:selected { color: %(text)s; background: %(panel)s;
    border-top: 2px solid %(accent)s; }
QTabBar::tab:hover:!selected { color: %(text)s; background: %(hover)s; }

/* --- status bar: dim text, with the current branch as an accent badge on the
       right like the mode badge in the opencode footer --- */
QStatusBar { background: %(bg)s; border-top: 1px solid %(border)s; color: %(dim)s; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: %(dim)s; padding: 0px 6px; }
QStatusBar QLabel#statusBranch { background: %(accent)s; color: %(onAccent)s;
    font-weight: 700; padding: 2px 10px; }
QStatusBar QToolButton { color: %(warning)s; }

QToolTip { background: %(panel)s; color: %(text)s; border: 1px solid %(borderStrong)s;
    padding: 4px 8px; }

QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }
QScrollBar::handle { background: %(borderStrong)s; border-radius: 0px; min-height: 28px;
    min-width: 28px; }
QScrollBar::handle:hover { background: %(dim)s; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QScrollArea { border: none; background: transparent; }
QProgressBar { background: %(element)s; border: none; text-align: center; max-height: 6px; }
QProgressBar::chunk { background: %(accent)s; }

QLabel { background: transparent; }
QLabel#searchSummary, QLabel#startPageSubtitle, QLabel#startPageRecentEmpty {
    color: %(dim)s; }
QLabel#startPageTitle { color: %(accent)s; }
QLabel#startPageRecentLabel { color: %(dim)s; }
)")
                            .replace(QStringLiteral("%(bg)s"), background().name())
                            .replace(QStringLiteral("%(panel)s"), panel().name())
                            .replace(QStringLiteral("%(surface)s"), surface().name())
                            .replace(QStringLiteral("%(elementHi)s"), elementHi.name())
                            .replace(QStringLiteral("%(element)s"), surfaceAlt().name())
                            .replace(QStringLiteral("%(border)s"), border().name())
                            .replace(QStringLiteral("%(borderStrong)s"), borderStrong().name())
                            .replace(QStringLiteral("%(text)s"), text().name())
                            .replace(QStringLiteral("%(dim)s"), textDim().name())
                            .replace(QStringLiteral("%(accent)s"), accent().name())
                            .replace(QStringLiteral("%(accentHi)s"), accent().lighter(110).name())
                            .replace(QStringLiteral("%(accentDim)s"), accent().darker(115).name())
                            .replace(QStringLiteral("%(selection)s"), selection().name())
                            .replace(QStringLiteral("%(warning)s"), warning().name())
                            .replace(QStringLiteral("%(danger)s"), danger().name())
                            .replace(QStringLiteral("%(dangerHi)s"), danger().lighter(115).name())
                            .replace(QStringLiteral("%(dangerFaded)s"), faded(danger()))
                            .replace(QStringLiteral("%(success)s"), success().name())
                            .replace(QStringLiteral("%(successHi)s"), success().lighter(115).name())
                            .replace(QStringLiteral("%(successFaded)s"), faded(success()))
                            .replace(QStringLiteral("%(onAccentFaded)s"), faint(onAccent()))
                            .replace(QStringLiteral("%(onAccent)s"), onAccent().name())
                            .replace(QStringLiteral("%(hover)s"), hover().name())
                            .replace(QStringLiteral("%(pressed)s"), pressed().name())
                            .replace(QStringLiteral("%(radiusSmall)px"),
                                     QString::number(radiusSmall()) + QStringLiteral("px"))
                            .replace(QStringLiteral("%(radius)px"),
                                     QString::number(radius()) + QStringLiteral("px"))
                            .replace(QStringLiteral("%(controlHeight)px"),
                                     QString::number(controlHeight()) + QStringLiteral("px"));
}

} // namespace gk::theme
