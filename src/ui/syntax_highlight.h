#pragma once

#include <QColor>
#include <QString>
#include <QVector>

namespace gk {

enum class Language {
    None,
    C,
    Cpp,
    CSharp,
    Java,
    JavaScript,
    TypeScript,
    Python,
    Go,
    Rust,
    Json,
    Yaml,
    Toml,
    Shell,
    Markdown,
    Cmake,
    Xml,
    Sql,
    Diff,
};

/// Maps a path to a language by its extension and, for the extensionless ones, by
/// name — `CMakeLists.txt` and `Makefile` are not guessable from a suffix.
Language languageForPath(const QString& path);

/// What a line leaves open for the next one. Block comments and triple-quoted
/// strings span lines, and a highlighter that forgets does so at every `/*`.
struct HighlightState {
    enum class Block { None, Comment, String };
    Block block = Block::None;
    QString terminator;
    bool tripleQuoted = false;
};

/// One stretch of a line and the colour it should be drawn in. Offsets are into the
/// raw line, before any escaping: escaping belongs to whoever builds the document.
struct SyntaxRun {
    int begin = 0;
    int length = 0;
    QColor colour;
};

/// Splits one line into coloured runs. `state` carries the block state across calls,
/// so lines must be fed in order.
QVector<SyntaxRun> highlightLine(const QString& line, Language language, HighlightState& state);

QColor colourAt(const QVector<SyntaxRun>& runs, int at);

/// HTML for one line's body: syntax colours, with `emphasis` ranges carrying a
/// background on top. The word-level diff is exactly such a range list, and the two
/// partitions are cut at every boundary so a coloured token inside a changed word
/// keeps both. Everything outside a run is escaped plain text.
QString renderHighlightedLine(const QString& line, Language language, HighlightState& state,
                              const QVector<QPair<int, int>>& emphasis = {},
                              const QString& emphasisBackground = QString());

} // namespace gk
