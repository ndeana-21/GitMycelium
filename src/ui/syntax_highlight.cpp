#include "ui/syntax_highlight.h"

#include "theme/theme.h"

#include <algorithm>

#include <QHash>
#include <QSet>
#include <QStringList>

namespace gk {
namespace {

/// Everything that varies between the languages, in one table. Adding a language is
/// adding a row; the tokeniser below reads only this.
struct LanguageRules {
    QSet<QString> keywords;
    QSet<QString> types;      ///< type names, coloured apart from other keywords
    QSet<QString> constants;  ///< true/false/null and friends
    QString lineComment;      ///< empty when the language has none
    QString blockStart;
    QString blockEnd;
    bool hashDirective = false;   ///< `#include`, `#!/bin/sh`
    bool properties = false;      ///< `key:` or `"key":` — the key is coloured apart
    bool tripleQuoted = false;    ///< Python's """ and '''
    bool markup = false;          ///< `<!-- -->`, tags, attributes
    bool markdown = false;
};

QSet<QString> setOf(std::initializer_list<const char*> words) {
    QSet<QString> out;
    for (const char* word : words) {
        out.insert(QString::fromLatin1(word));
    }
    return out;
}

const LanguageRules* rulesFor(Language language) {
    static const QHash<int, LanguageRules> table = [] {
        QHash<int, LanguageRules> t;

        LanguageRules cLike;
        cLike.keywords = setOf({"if", "else", "for", "while", "do", "switch", "case", "default",
                                "break", "continue", "return", "goto", "throw", "try", "catch",
                                "new", "delete", "sizeof", "static", "const", "constexpr",
                                "volatile", "extern", "inline", "virtual", "override", "final",
                                "public", "private", "protected", "class", "struct", "union",
                                "enum", "namespace", "using", "template", "typename", "typedef",
                                "operator", "this", "nullptr", "true", "false", "auto", "friend",
                                "explicit", "mutable", "noexcept", "static_cast", "dynamic_cast",
                                "const_cast", "reinterpret_cast"});
        cLike.types = setOf({"void", "bool", "char", "short", "int", "long", "float", "double",
                             "signed", "unsigned", "size_t", "uint8_t", "uint16_t", "uint32_t",
                             "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "wchar_t",
                             "std", "string", "vector", "map", "set", "pair"});
        cLike.lineComment = QStringLiteral("//");
        cLike.blockStart = QStringLiteral("/*");
        cLike.blockEnd = QStringLiteral("*/");
        cLike.hashDirective = true;
        t.insert(static_cast<int>(Language::Cpp), cLike);
        t.insert(static_cast<int>(Language::C), cLike);

        LanguageRules java = cLike;
        java.keywords = setOf({"if", "else", "for", "while", "do", "switch", "case", "default",
                               "break", "continue", "return", "throw", "throws", "try", "catch",
                               "finally", "new", "static", "final", "abstract", "synchronized",
                               "volatile", "transient", "native", "public", "private", "protected",
                               "class", "interface", "enum", "record", "extends", "implements",
                               "package", "import", "this", "super", "null", "true", "false",
                               "instanceof", "assert", "var", "sealed", "permits", "yield"});
        java.types = setOf({"void", "boolean", "byte", "char", "short", "int", "long", "float",
                            "double", "String", "Object", "List", "Map", "Set", "Integer",
                            "Long", "Double", "Boolean", "Optional", "Stream"});
        java.hashDirective = false;
        t.insert(static_cast<int>(Language::Java), java);

        LanguageRules csharp = java;
        csharp.keywords = setOf({"if", "else", "for", "foreach", "while", "do", "switch", "case",
                                 "default", "break", "continue", "return", "throw", "try", "catch",
                                 "finally", "new", "static", "readonly", "const", "virtual",
                                 "override", "abstract", "sealed", "partial", "async", "await",
                                 "public", "private", "protected", "internal", "class", "struct",
                                 "interface", "enum", "record", "namespace", "using", "this",
                                 "base", "null", "true", "false", "is", "as", "in", "out", "ref",
                                 "params", "yield", "nameof", "lock", "get", "set", "value"});
        csharp.types = setOf({"void", "bool", "byte", "sbyte", "char", "short", "ushort", "int",
                              "uint", "long", "ulong", "float", "double", "decimal", "string",
                              "object", "dynamic", "var", "Task", "List", "Dictionary", "IEnumerable",
                              "Exception", "string"});
        t.insert(static_cast<int>(Language::CSharp), csharp);

        LanguageRules js;
        js.keywords = setOf({"if", "else", "for", "while", "do", "switch", "case", "default",
                             "break", "continue", "return", "throw", "try", "catch", "finally",
                             "new", "delete", "typeof", "instanceof", "in", "of", "void",
                             "function", "class", "extends", "super", "this", "const", "let",
                             "var", "async", "await", "yield", "import", "export", "from", "as",
                             "default", "static", "get", "set", "interface", "type", "enum",
                             "implements", "public", "private", "protected", "readonly", "declare",
                             "namespace", "satisfies", "keyof", "infer"});
        js.constants = setOf({"null", "undefined", "true", "false", "NaN", "Infinity"});
        js.lineComment = QStringLiteral("//");
        js.blockStart = QStringLiteral("/*");
        js.blockEnd = QStringLiteral("*/");
        js.properties = true;
        t.insert(static_cast<int>(Language::JavaScript), js);
        t.insert(static_cast<int>(Language::TypeScript), js);

        LanguageRules python;
        python.keywords = setOf({"if", "elif", "else", "for", "while", "break", "continue",
                                 "return", "pass", "raise", "try", "except", "finally", "with",
                                 "as", "def", "class", "lambda", "global", "nonlocal", "import",
                                 "from", "del", "assert", "yield", "async", "await", "match",
                                 "case", "and", "or", "not", "is", "in", "self", "cls"});
        python.constants = setOf({"True", "False", "None", "NotImplemented", "Ellipsis"});
        python.types = setOf({"int", "float", "str", "bool", "bytes", "list", "dict", "set",
                              "tuple", "object", "type", "any", "Exception", "ValueError",
                              "TypeError", "KeyError", "RuntimeError", "Optional", "List", "Dict"});
        python.lineComment = QStringLiteral("#");
        python.properties = true;
        python.tripleQuoted = true;
        t.insert(static_cast<int>(Language::Python), python);

        LanguageRules go;
        go.keywords = setOf({"if", "else", "for", "switch", "case", "default", "break", "continue",
                             "return", "goto", "fallthrough", "defer", "go", "select", "chan",
                             "func", "var", "const", "type", "struct", "interface", "map", "range",
                             "package", "import", "nil", "true", "false", "iota"});
        go.types = setOf({"bool", "byte", "rune", "string", "int", "int8", "int16", "int32",
                          "int64", "uint", "uint8", "uint16", "uint32", "uint64", "uintptr",
                          "float32", "float64", "complex64", "complex128", "error", "any"});
        go.lineComment = QStringLiteral("//");
        go.blockStart = QStringLiteral("/*");
        go.blockEnd = QStringLiteral("*/");
        t.insert(static_cast<int>(Language::Go), go);

        LanguageRules rust;
        rust.keywords = setOf({"if", "else", "for", "while", "loop", "match", "break", "continue",
                               "return", "fn", "let", "mut", "const", "static", "struct", "enum",
                               "trait", "impl", "for", "in", "pub", "use", "mod", "crate", "super",
                               "self", "Self", "where", "as", "dyn", "box", "move", "ref", "async",
                               "await", "unsafe", "extern", "type", "true", "false"});
        rust.types = setOf({"i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64",
                            "u128", "usize", "f32", "f64", "bool", "char", "str", "String", "Vec",
                            "Option", "Result", "Box", "Rc", "Arc", "HashMap", "Some", "None",
                            "Ok", "Err"});
        rust.lineComment = QStringLiteral("//");
        rust.blockStart = QStringLiteral("/*");
        rust.blockEnd = QStringLiteral("*/");
        rust.properties = true;
        t.insert(static_cast<int>(Language::Rust), rust);

        LanguageRules json;
        json.constants = setOf({"true", "false", "null"});
        json.properties = true;
        t.insert(static_cast<int>(Language::Json), json);

        LanguageRules yaml;
        yaml.constants = setOf({"true", "false", "null", "yes", "no", "on", "off", "~"});
        yaml.lineComment = QStringLiteral("#");
        yaml.properties = true;
        t.insert(static_cast<int>(Language::Yaml), yaml);

        LanguageRules toml = yaml;
        toml.lineComment = QStringLiteral("#");
        t.insert(static_cast<int>(Language::Toml), toml);

        LanguageRules shell;
        shell.keywords = setOf({"if", "then", "else", "elif", "fi", "for", "while", "until", "do",
                                "done", "case", "esac", "in", "function", "return", "exit", "local",
                                "export", "readonly", "declare", "source", "set", "unset", "shift",
                                "trap", "eval", "exec", "let", "test"});
        shell.lineComment = QStringLiteral("#");
        shell.hashDirective = true;
        t.insert(static_cast<int>(Language::Shell), shell);

        LanguageRules cmake;
        cmake.lineComment = QStringLiteral("#");
        t.insert(static_cast<int>(Language::Cmake), cmake);

        LanguageRules markup;
        markup.markup = true;
        markup.blockStart = QStringLiteral("<!--");
        markup.blockEnd = QStringLiteral("-->");
        t.insert(static_cast<int>(Language::Xml), markup);

        LanguageRules markdown;
        markdown.markdown = true;
        t.insert(static_cast<int>(Language::Markdown), markdown);

        LanguageRules sql;
        sql.keywords = setOf({"select", "from", "where", "insert", "into", "values", "update",
                              "set", "delete", "create", "table", "index", "drop", "alter", "join",
                              "left", "right", "inner", "outer", "on", "group", "by", "order",
                              "having", "limit", "offset", "union", "distinct", "as", "and", "or",
                              "not", "null", "is", "in", "between", "like", "case", "when", "then",
                              "else", "end", "primary", "key", "foreign", "references", "default"});
        sql.constants = setOf({"true", "false", "null"});
        sql.lineComment = QStringLiteral("--");
        sql.blockStart = QStringLiteral("/*");
        sql.blockEnd = QStringLiteral("*/");
        t.insert(static_cast<int>(Language::Sql), sql);

        return t;
    }();
    // A pointer, not a reference: QHash::value() returns a copy, and handing back a
    // reference to it would dangle the moment the caller looked at a keyword.
    static const LanguageRules empty;
    const auto it = table.constFind(static_cast<int>(language));
    return it == table.constEnd() ? &empty : &(*it);
}

bool isIdentifierStart(QChar c) {
    return c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

bool isIdentifierPart(QChar c) {
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

void add(QVector<SyntaxRun>& runs, int begin, int length, const QColor& colour) {
    if (length <= 0) {
        return;
    }
    if (!runs.isEmpty() && runs.last().colour == colour &&
        runs.last().begin + runs.last().length == begin) {
        runs.last().length += length;
        return;
    }
    SyntaxRun run;
    run.begin = begin;
    run.length = length;
    run.colour = colour;
    runs.append(run);
}

int stringEnd(const QString& line, int at, QChar quote) {
    int i = at + 1;
    while (i < line.size()) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('\\')) {
            i += 2;
            continue;
        }
        if (c == quote) {
            return i + 1;
        }
        ++i;
    }
    return line.size();
}

QVector<SyntaxRun> markdownLine(const QString& line) {
    QVector<SyntaxRun> runs;
    if (line.startsWith(QLatin1String("```")) || line.startsWith(QLatin1String("~~~"))) {
        add(runs, 0, line.size(), theme::syntaxDirective());
        return runs;
    }
    int hashes = 0;
    while (hashes < line.size() && line.at(hashes) == QLatin1Char('#')) {
        ++hashes;
    }
    if (hashes > 0 && hashes < line.size() && line.at(hashes) == QLatin1Char(' ')) {
        add(runs, 0, line.size(), theme::syntaxKeyword());
        return runs;
    }
    if (line.startsWith(QLatin1String("> "))) {
        add(runs, 0, line.size(), theme::syntaxComment());
        return runs;
    }
    if (line.trimmed().startsWith(QLatin1String("- ")) ||
        line.trimmed().startsWith(QLatin1String("* ")) ||
        line.trimmed().startsWith(QLatin1String("+ "))) {
        const int bullet = line.indexOf(QLatin1Char('-'), 0) >= 0 ? line.indexOf(QLatin1Char('-'))
                                                                  : line.indexOf(QLatin1Char('*'));
        if (bullet >= 0) {
            add(runs, bullet, 1, theme::syntaxKeyword());
        }
        return runs;
    }
    int i = 0;
    while (i < line.size()) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('`')) {
            int end = line.indexOf(QLatin1Char('`'), i + 1);
            end = end < 0 ? line.size() : end + 1;
            add(runs, i, end - i, theme::syntaxString());
            i = end;
            continue;
        }
        if (c == QLatin1Char('*') || c == QLatin1Char('_')) {
            int end = line.indexOf(c, i + 1);
            if (end > i + 1) {
                add(runs, i, end - i + 1, theme::syntaxType());
                i = end + 1;
                continue;
            }
        }
        ++i;
    }
    return runs;
}

QVector<SyntaxRun> markupLine(const QString& line, HighlightState& state, const LanguageRules& rules) {
    QVector<SyntaxRun> runs;
    int i = 0;
    while (i < line.size()) {
        if (state.block == HighlightState::Block::Comment) {
            const int end = line.indexOf(rules.blockEnd, i);
            const int stop = end < 0 ? line.size() : end + rules.blockEnd.size();
            add(runs, i, stop - i, theme::syntaxComment());
            if (end >= 0) {
                state = HighlightState{};
            } else {
                return runs;
            }
            i = stop;
            continue;
        }
        if (line.mid(i, rules.blockStart.size()) == rules.blockStart && !rules.blockStart.isEmpty()) {
            state.block = HighlightState::Block::Comment;
            state.terminator = rules.blockEnd;
            continue;
        }
        const QChar c = line.at(i);
        if (c == QLatin1Char('<')) {
            const int end = line.indexOf(QLatin1Char('>'), i);
            const int stop = end < 0 ? line.size() : end + 1;
            int j = i;
            while (j < stop) {
                const QChar t = line.at(j);
                if (t == QLatin1Char('"') || t == QLatin1Char('\'')) {
                    const int quoted = stringEnd(line, j, t);
                    add(runs, j, qMin(quoted, stop) - j, theme::syntaxString());
                    j = qMin(quoted, stop);
                    continue;
                }
                if (isIdentifierStart(t) || t == QLatin1Char('/')) {
                    int k = j;
                    while (k < stop && (isIdentifierPart(line.at(k)) || line.at(k) == QLatin1Char('/'))) {
                        ++k;
                    }
                    add(runs, j, k - j, theme::syntaxKeyword());
                    j = k;
                    continue;
                }
                ++j;
            }
            i = stop;
            continue;
        }
        ++i;
    }
    return runs;
}

} // namespace

Language languageForPath(const QString& path) {
    const QString name = path.section(QLatin1Char('/'), -1).toLower();
    if (name == QLatin1String("cmakelists.txt") || name == QLatin1String("cmakecache.txt") ||
        name.endsWith(QLatin1String(".cmake"))) {
        return Language::Cmake;
    }
    if (name == QLatin1String("makefile") || name == QLatin1String("gnumakefile")) {
        return Language::Cmake;
    }
    if (name == QLatin1String("dockerfile") || name.startsWith(QLatin1String("dockerfile."))) {
        return Language::Shell;
    }
    if (name.endsWith(QLatin1String(".h")) || name.endsWith(QLatin1String(".hpp")) ||
        name.endsWith(QLatin1String(".hh")) || name.endsWith(QLatin1String(".hxx")) ||
        name.endsWith(QLatin1String(".cc")) || name.endsWith(QLatin1String(".cpp")) ||
        name.endsWith(QLatin1String(".cxx")) || name.endsWith(QLatin1String(".c++")) ||
        name.endsWith(QLatin1String(".ipp"))) {
        return Language::Cpp;
    }
    if (name.endsWith(QLatin1String(".c"))) {
        return Language::C;
    }
    if (name.endsWith(QLatin1String(".java"))) {
        return Language::Java;
    }
    if (name.endsWith(QLatin1String(".cs"))) {
        return Language::CSharp;
    }
    if (name.endsWith(QLatin1String(".ts")) || name.endsWith(QLatin1String(".tsx"))) {
        return Language::TypeScript;
    }
    if (name.endsWith(QLatin1String(".js")) || name.endsWith(QLatin1String(".jsx")) ||
        name.endsWith(QLatin1String(".mjs")) || name.endsWith(QLatin1String(".cjs"))) {
        return Language::JavaScript;
    }
    if (name.endsWith(QLatin1String(".py")) || name.endsWith(QLatin1String(".pyi"))) {
        return Language::Python;
    }
    if (name.endsWith(QLatin1String(".go"))) {
        return Language::Go;
    }
    if (name.endsWith(QLatin1String(".rs"))) {
        return Language::Rust;
    }
    if (name.endsWith(QLatin1String(".json")) || name.endsWith(QLatin1String(".jsonc")) ||
        name.endsWith(QLatin1String(".ipynb"))) {
        return Language::Json;
    }
    if (name.endsWith(QLatin1String(".yaml")) || name.endsWith(QLatin1String(".yml"))) {
        return Language::Yaml;
    }
    if (name.endsWith(QLatin1String(".toml"))) {
        return Language::Toml;
    }
    if (name.endsWith(QLatin1String(".sh")) || name.endsWith(QLatin1String(".bash")) ||
        name.endsWith(QLatin1String(".zsh")) || name.endsWith(QLatin1String(".fish"))) {
        return Language::Shell;
    }
    if (name.endsWith(QLatin1String(".md")) || name.endsWith(QLatin1String(".markdown"))) {
        return Language::Markdown;
    }
    if (name.endsWith(QLatin1String(".xml")) || name.endsWith(QLatin1String(".html")) ||
        name.endsWith(QLatin1String(".htm")) || name.endsWith(QLatin1String(".svg")) ||
        name.endsWith(QLatin1String(".ui")) || name.endsWith(QLatin1String(".qrc"))) {
        return Language::Xml;
    }
    if (name.endsWith(QLatin1String(".sql"))) {
        return Language::Sql;
    }
    if (name.endsWith(QLatin1String(".conf")) || name.endsWith(QLatin1String(".ini")) ||
        name.endsWith(QLatin1String(".env")) || name.endsWith(QLatin1String(".properties"))) {
        return Language::Toml;
    }
    return Language::None;
}

QVector<SyntaxRun> highlightLine(const QString& line, Language language, HighlightState& state) {
    QVector<SyntaxRun> runs;
    if (language == Language::None) {
        return runs;
    }
    if (language == Language::Markdown && state.block == HighlightState::Block::None) {
        return markdownLine(line);
    }

    const LanguageRules& rules = *rulesFor(language);
    if (rules.markup && state.block != HighlightState::Block::String) {
        return markupLine(line, state, rules);
    }

    const QColor keyword = theme::syntaxKeyword();
    const QColor type = theme::syntaxType();
    const QColor function = theme::syntaxFunction();
    const QColor string = theme::syntaxString();
    const QColor number = theme::syntaxNumber();
    const QColor comment = theme::syntaxComment();
    const QColor property = theme::syntaxProperty();
    const QColor directive = theme::syntaxDirective();
    const QColor constant = theme::syntaxDirective();

    int i = 0;
    while (i < line.size()) {
        if (state.block == HighlightState::Block::Comment) {
            const int end = line.indexOf(state.terminator, i);
            const int stop = end < 0 ? line.size() : end + state.terminator.size();
            add(runs, i, stop - i, comment);
            if (end < 0) {
                return runs;
            }
            state = HighlightState{};
            i = stop;
            continue;
        }
        if (state.block == HighlightState::Block::String) {
            const int end = line.indexOf(state.terminator, i);
            const int stop = end < 0 ? line.size() : end + state.terminator.size();
            add(runs, i, stop - i, string);
            if (end < 0) {
                return runs;
            }
            state = HighlightState{};
            i = stop;
            continue;
        }

        const QChar c = line.at(i);

        // A preprocessor line: `#include`, a shebang. It has to start the line — in
        // shell a `#` further in is an ordinary comment, and in CMake every `#` is.
        if (rules.hashDirective && c == QLatin1Char('#') &&
            line.left(i).trimmed().isEmpty()) {
            add(runs, i, line.size() - i, directive);
            return runs;
        }
        if (!rules.lineComment.isEmpty() && line.mid(i, rules.lineComment.size()) == rules.lineComment) {
            add(runs, i, line.size() - i, comment);
            return runs;
        }
        if (!rules.blockStart.isEmpty() && line.mid(i, rules.blockStart.size()) == rules.blockStart) {
            const int end = line.indexOf(rules.blockEnd, i + rules.blockStart.size());
            const int stop = end < 0 ? line.size() : end + rules.blockEnd.size();
            add(runs, i, stop - i, comment);
            if (end < 0) {
                state.block = HighlightState::Block::Comment;
                state.terminator = rules.blockEnd;
                return runs;
            }
            i = stop;
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`')) {
            if (rules.tripleQuoted && line.mid(i, 3) == QString(c).repeated(3)) {
                const QString terminator = QString(c).repeated(3);
                const int end = line.indexOf(terminator, i + 3);
                const int stop = end < 0 ? line.size() : end + 3;
                add(runs, i, stop - i, string);
                if (end < 0) {
                    state.block = HighlightState::Block::String;
                    state.terminator = terminator;
                    state.tripleQuoted = true;
                    return runs;
                }
                i = stop;
                continue;
            }
            const int end = stringEnd(line, i, c);
            int after = end;
            while (after < line.size() && line.at(after) == QLatin1Char(' ')) {
                ++after;
            }
            const bool key = rules.properties && after < line.size() &&
                             line.at(after) == QLatin1Char(':');
            add(runs, i, end - i, key ? property : string);
            i = end;
            continue;
        }
        if (c.isDigit() && (i == 0 || !isIdentifierPart(line.at(i - 1)))) {
            int j = i;
            while (j < line.size()) {
                const QChar n = line.at(j);
                if (n.isLetterOrNumber() || n == QLatin1Char('.') || n == QLatin1Char('_') ||
                    n == QLatin1Char('x') || n == QLatin1Char('X')) {
                    ++j;
                    continue;
                }
                break;
            }
            add(runs, i, j - i, number);
            i = j;
            continue;
        }
        if (isIdentifierStart(c)) {
            int j = i;
            while (j < line.size() && isIdentifierPart(line.at(j))) {
                ++j;
            }
            const QString word = line.mid(i, j - i);
            int after = j;
            while (after < line.size() && line.at(after) == QLatin1Char(' ')) {
                ++after;
            }
            const bool call = after < line.size() && line.at(after) == QLatin1Char('(');
            const bool key = rules.properties && after < line.size() &&
                             (line.at(after) == QLatin1Char(':'));
            QColor colour;
            if (rules.keywords.contains(word)) {
                colour = keyword;
            } else if (rules.constants.contains(word)) {
                colour = constant;
            } else if (rules.types.contains(word)) {
                colour = type;
            } else if (key) {
                colour = property;
            } else if (call) {
                colour = function;
            } else if (!word.isEmpty() && word.at(0).isUpper() && word.size() > 1) {
                colour = type;
            }
            if (colour.isValid()) {
                add(runs, i, j - i, colour);
            }
            i = j;
            continue;
        }
        ++i;
    }
    return runs;
}

QString renderHighlightedLine(const QString& line, Language language, HighlightState& state,
                              const QVector<QPair<int, int>>& emphasis,
                              const QString& emphasisBackground) {
    const QVector<SyntaxRun> runs = highlightLine(line, language, state);

    // Every offset where the colour or the emphasis changes. Cutting at all of them
    // is what lets a token and a changed word overlap without nesting one inside the
    // other: each piece is wrapped once, with both attributes if it has both.
    QVector<int> cuts;
    cuts.append(0);
    cuts.append(line.size());
    for (const SyntaxRun& run : runs) {
        cuts.append(run.begin);
        cuts.append(run.begin + run.length);
    }
    for (const QPair<int, int>& range : emphasis) {
        cuts.append(range.first);
        cuts.append(range.first + range.second);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    auto emphasised = [&emphasis](int at) {
        for (const QPair<int, int>& range : emphasis) {
            if (at >= range.first && at < range.first + range.second) {
                return true;
            }
        }
        return false;
    };

    QString html;
    QString pendingStyle;
    QString pendingText;
    auto flush = [&html, &pendingStyle, &pendingText] {
        if (pendingText.isEmpty()) {
            return;
        }
        const QString escapedText = pendingText.toHtmlEscaped();
        html += pendingStyle.isEmpty()
                    ? escapedText
                    : QStringLiteral("<span style=\"%1\">%2</span>").arg(pendingStyle, escapedText);
        pendingText.clear();
    };

    for (int i = 0; i + 1 < cuts.size(); ++i) {
        const int begin = cuts.at(i);
        const int end = cuts.at(i + 1);
        if (end <= begin) {
            continue;
        }
        const QColor colour = colourAt(runs, begin);
        const bool hot = emphasised(begin) && !emphasisBackground.isEmpty();
        QString style;
        if (colour.isValid() && hot) {
            style = QStringLiteral("color:%1; background:%2").arg(colour.name(), emphasisBackground);
        } else if (colour.isValid()) {
            style = QStringLiteral("color:%1").arg(colour.name());
        } else if (hot) {
            style = QStringLiteral("background:%1").arg(emphasisBackground);
        }
        if (style != pendingStyle) {
            flush();
            pendingStyle = style;
        }
        pendingText += line.mid(begin, end - begin);
    }
    flush();
    return html;
}

QColor colourAt(const QVector<SyntaxRun>& runs, int at) {
    for (const SyntaxRun& run : runs) {
        if (at >= run.begin && at < run.begin + run.length) {
            return run.colour;
        }
    }
    return QColor();
}

} // namespace gk
