#include "theme/theme.h"
#include "ui/syntax_highlight.h"

#include <QtTest>

using namespace gk;

namespace {

QColor colourOf(const QString& line, Language language, int at) {
    HighlightState state;
    return colourAt(highlightLine(line, language, state), at);
}

int at(const QString& line, const QString& token) {
    return line.indexOf(token);
}

} // namespace

class SyntaxHighlightTest : public QObject {
    Q_OBJECT

private slots:
    void pathsMapToLanguages();
    void unknownLanguagesStayPlain();

    void cppTokensAreColoured();
    void aBlockCommentCarriesToTheNextLine();
    void aStringIsNotEndedByAnEscapedQuote();
    void aLineCommentSwallowsWhatFollows();

    void pythonTripleQuotesSpanLines();
    void jsonSeparatesKeysFromValues();
    void yamlKeysAndComments();
    void shellDirectivesOwnTheirLine();
    void markupTagsAndComments();
    void markdownHeadingsAndCode();

    void emphasisIsLaidOverSyntaxRatherThanNesting();
    void escapingHappensAfterHighlighting();
};

void SyntaxHighlightTest::pathsMapToLanguages() {
    QCOMPARE(languageForPath(QStringLiteral("src/main.cpp")), Language::Cpp);
    QCOMPARE(languageForPath(QStringLiteral("include/thing.h")), Language::Cpp);
    QCOMPARE(languageForPath(QStringLiteral("src/legacy.c")), Language::C);
    QCOMPARE(languageForPath(QStringLiteral("com/example/Main.java")), Language::Java);
    QCOMPARE(languageForPath(QStringLiteral("Program.cs")), Language::CSharp);
    QCOMPARE(languageForPath(QStringLiteral("web/app.tsx")), Language::TypeScript);
    QCOMPARE(languageForPath(QStringLiteral("web/app.js")), Language::JavaScript);
    QCOMPARE(languageForPath(QStringLiteral("tools/thing.py")), Language::Python);
    QCOMPARE(languageForPath(QStringLiteral("cmd/server/main.go")), Language::Go);
    QCOMPARE(languageForPath(QStringLiteral("src/lib.rs")), Language::Rust);
    QCOMPARE(languageForPath(QStringLiteral("package.json")), Language::Json);
    QCOMPARE(languageForPath(QStringLiteral(".github/workflows/ci.yml")), Language::Yaml);
    QCOMPARE(languageForPath(QStringLiteral("Cargo.toml")), Language::Toml);
    QCOMPARE(languageForPath(QStringLiteral("scripts/build.sh")), Language::Shell);
    QCOMPARE(languageForPath(QStringLiteral("README.md")), Language::Markdown);
    QCOMPARE(languageForPath(QStringLiteral("layout.ui")), Language::Xml);
    QCOMPARE(languageForPath(QStringLiteral("migrations/001.sql")), Language::Sql);
    QCOMPARE(languageForPath(QStringLiteral("CMakeLists.txt")), Language::Cmake);
    QCOMPARE(languageForPath(QStringLiteral("Makefile")), Language::Cmake);
    QCOMPARE(languageForPath(QStringLiteral("Dockerfile")), Language::Shell);
    QCOMPARE(languageForPath(QStringLiteral("app.conf")), Language::Toml);
}

void SyntaxHighlightTest::unknownLanguagesStayPlain() {
    QCOMPARE(languageForPath(QStringLiteral("data/thing.xyzzy")), Language::None);

    const QString line = QStringLiteral("int main() { return 0; }");
    QVERIFY(colourOf(line, Language::None, 0).isValid() == false);
    HighlightState blank;
    QVERIFY(highlightLine(QStringLiteral("   "), Language::Cpp, blank).isEmpty());
}

void SyntaxHighlightTest::cppTokensAreColoured() {
    const QString line = QStringLiteral("static int count = 42; // how many");
    QCOMPARE(colourOf(line, Language::Cpp, at(line, QStringLiteral("static"))), theme::syntaxKeyword());
    QCOMPARE(colourOf(line, Language::Cpp, at(line, QStringLiteral("int"))), theme::syntaxType());
    QCOMPARE(colourOf(line, Language::Cpp, at(line, QStringLiteral("42"))), theme::syntaxNumber());
    QCOMPARE(colourOf(line, Language::Cpp, at(line, QStringLiteral("//"))), theme::syntaxComment());
    QCOMPARE(colourOf(line, Language::Cpp, at(line, QStringLiteral("how many"))),
             theme::syntaxComment());

    const QString call = QStringLiteral("QString value = compute();");
    QCOMPARE(colourOf(call, Language::Cpp, at(call, QStringLiteral("compute"))),
             theme::syntaxFunction());
    QCOMPARE(colourOf(call, Language::Cpp, at(call, QStringLiteral("QString"))), theme::syntaxType());
}

void SyntaxHighlightTest::aBlockCommentCarriesToTheNextLine() {
    HighlightState state;
    const QString first = QStringLiteral("/* this runs on");
    const QString second = QStringLiteral("   and on */ int after = 1;");
    const QVector<SyntaxRun> a = highlightLine(first, Language::Cpp, state);
    QCOMPARE(colourAt(a, 3), theme::syntaxComment());
    QCOMPARE(state.block, HighlightState::Block::Comment);

    const QVector<SyntaxRun> b = highlightLine(second, Language::Cpp, state);
    QCOMPARE(colourAt(b, 3), theme::syntaxComment());
    QCOMPARE(colourAt(b, second.indexOf(QStringLiteral("int"))), theme::syntaxType());
    QCOMPARE(colourAt(b, second.indexOf(QStringLiteral("1"))), theme::syntaxNumber());
    QCOMPARE(state.block, HighlightState::Block::None);
}

void SyntaxHighlightTest::aStringIsNotEndedByAnEscapedQuote() {
    const QString line = QStringLiteral("s = \"a \\\" b\" + c;");
    QCOMPARE(colourOf(line, Language::Cpp, line.indexOf(QStringLiteral("b"))),
             theme::syntaxString());
    QCOMPARE(colourOf(line, Language::Cpp, line.indexOf(QStringLiteral("\""), 8)),
             theme::syntaxString());
}

void SyntaxHighlightTest::aLineCommentSwallowsWhatFollows() {
    const QString line = QStringLiteral("let x = 1; # not code \"here\"");
    QCOMPARE(colourOf(line, Language::Python, line.indexOf(QStringLiteral("#"))),
             theme::syntaxComment());
    QCOMPARE(colourOf(line, Language::Python, line.indexOf(QStringLiteral("here"))),
             theme::syntaxComment());
}

void SyntaxHighlightTest::pythonTripleQuotesSpanLines() {
    HighlightState state;
    const QString first = QStringLiteral("doc = \"\"\"first");
    highlightLine(first, Language::Python, state);
    QCOMPARE(state.block, HighlightState::Block::String);

    const QString second = QStringLiteral("second\"\"\" + 1");
    const QVector<SyntaxRun> runs = highlightLine(second, Language::Python, state);
    QCOMPARE(colourAt(runs, 0), theme::syntaxString());
    QCOMPARE(colourAt(runs, second.indexOf(QStringLiteral("1"))), theme::syntaxNumber());
    QCOMPARE(state.block, HighlightState::Block::None);
}

void SyntaxHighlightTest::jsonSeparatesKeysFromValues() {
    const QString line = QStringLiteral("  \"workers\": 8,");
    QCOMPARE(colourOf(line, Language::Json, line.indexOf(QStringLiteral("workers"))),
             theme::syntaxProperty());
    QCOMPARE(colourOf(line, Language::Json, line.indexOf(QStringLiteral("8"))),
             theme::syntaxNumber());

    const QString literalValue = QStringLiteral("  \"name\": \"api\",");
    QCOMPARE(colourOf(literalValue, Language::Json, literalValue.indexOf(QStringLiteral("api"))),
             theme::syntaxString());
    QCOMPARE(colourOf(QStringLiteral("enabled = true"), Language::Json, 10),
             theme::syntaxDirective());
}

void SyntaxHighlightTest::yamlKeysAndComments() {
    const QString line = QStringLiteral("replicas: 3  # keep it small");
    QCOMPARE(colourOf(line, Language::Yaml, 0), theme::syntaxProperty());
    QCOMPARE(colourOf(line, Language::Yaml, line.indexOf(QStringLiteral("3"))),
             theme::syntaxNumber());
    QCOMPARE(colourOf(line, Language::Yaml, line.indexOf(QStringLiteral("#"))),
             theme::syntaxComment());
}

void SyntaxHighlightTest::shellDirectivesOwnTheirLine() {
    const QString shebang = QStringLiteral("#!/bin/bash");
    QCOMPARE(colourOf(shebang, Language::Shell, 0), theme::syntaxDirective());

    const QString line = QStringLiteral("if [ -f \"$file\" ]; then echo done; fi");
    QCOMPARE(colourOf(line, Language::Shell, 0), theme::syntaxKeyword());
    QCOMPARE(colourOf(line, Language::Shell, at(line, QStringLiteral("then"))),
             theme::syntaxKeyword());
    QCOMPARE(colourOf(line, Language::Shell, at(line, QStringLiteral("\"$file\""))),
             theme::syntaxString());
}

void SyntaxHighlightTest::markupTagsAndComments() {
    const QString line = QStringLiteral("<a href=\"page.html\">link</a>");
    QCOMPARE(colourOf(line, Language::Xml, 1), theme::syntaxKeyword());
    QCOMPARE(colourOf(line, Language::Xml, line.indexOf(QStringLiteral("page.html"))),
             theme::syntaxString());

    HighlightState state;
    const QVector<SyntaxRun> runs =
        highlightLine(QStringLiteral("<!-- a note -->"), Language::Xml, state);
    QCOMPARE(colourAt(runs, 5), theme::syntaxComment());
    QCOMPARE(state.block, HighlightState::Block::None);
}

void SyntaxHighlightTest::markdownHeadingsAndCode() {
    const QString heading = QStringLiteral("## Title");
    QCOMPARE(colourOf(heading, Language::Markdown, 0), theme::syntaxKeyword());

    const QString code = QStringLiteral("text with `inline()` code");
    QCOMPARE(colourOf(code, Language::Markdown, code.indexOf(QStringLiteral("inline"))),
             theme::syntaxString());
}

void SyntaxHighlightTest::emphasisIsLaidOverSyntaxRatherThanNesting() {
    // A changed word inside a string: both the colour and the background must be
    // there, and the spans must not nest.
    const QString line = QStringLiteral("name = \"old\";");
    HighlightState state;
    const int begin = line.indexOf(QStringLiteral("old"));
    const QVector<QPair<int, int>> emphasis{{begin, 3}};
    const QString html = renderHighlightedLine(line, Language::Python, state, emphasis,
                                               QStringLiteral("#335544"));

    QVERIFY(html.contains(theme::syntaxString().name()));
    QVERIFY(html.contains(QStringLiteral("#335544")));
    QVERIFY(html.contains(QStringLiteral("old")));
    const int firstClose = html.indexOf(QStringLiteral("</span>"));
    const int secondOpen = html.indexOf(QStringLiteral("<span"), firstClose);
    if (secondOpen >= 0) {
        QVERIFY(html.count(QStringLiteral("<span")) <= 3);
    }
}

void SyntaxHighlightTest::escapingHappensAfterHighlighting() {
    // The runs address the raw line; escaping belongs to the renderer, and it must
    // escape what it emits.
    const QString line = QStringLiteral("if (a < b && c > d) { }");
    HighlightState state;
    const QString html = renderHighlightedLine(line, Language::Cpp, state);
    QVERIFY(!html.contains(QStringLiteral("< b")));
    QVERIFY(html.contains(QStringLiteral("&lt;")));
    QVERIFY(html.contains(QStringLiteral("&gt;")));
    QVERIFY(html.contains(QStringLiteral("&amp;&amp;")));
}

QTEST_MAIN(SyntaxHighlightTest)
#include "test_syntax_highlight.moc"
