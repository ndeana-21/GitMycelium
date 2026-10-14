/// Choosing the terminal emulator command. The lookup is injected, so the test does
/// not depend on what is installed: it decides which names resolve and asserts the
/// arguments each emulator gets.

#include "core/external_terminal.h"

#include <QtTest>

#include <algorithm>
#include <memory>
#include <set>

using namespace gk;

namespace {

std::function<std::string(const std::string&)> resolverFor(
    std::initializer_list<const char*> installed) {
    auto names = std::make_shared<std::set<std::string>>();
    for (const char* name : installed) {
        names->insert(name);
    }
    return [names](const std::string& name) -> std::string {
        return names->count(name) > 0 ? "/usr/bin/" + name : std::string();
    };
}

bool hasArg(const TerminalLaunch& launch, const std::string& value) {
    return std::find(launch.args.begin(), launch.args.end(), value) != launch.args.end();
}

} // namespace

class ExternalTerminalTest : public QObject {
    Q_OBJECT

private slots:
    void thePreferredTerminalWins();
    void konsoleGetsItsWorkdirFlag();
    void gnomeTerminalUsesTheEqualsForm();
    void xtermTakesTheGenericForm();
    void anUnrecognisedTerminalTakesTheGenericForm();
    void noTerminalIsHonestlyInvalid();
};

void ExternalTerminalTest::thePreferredTerminalWins() {
    const TerminalLaunch launch = buildTerminalLaunch("/repo", "myterm", resolverFor({"konsole", "myterm"}));
    QVERIFY(launch.valid);
    QCOMPARE(QString::fromStdString(launch.program), QStringLiteral("/usr/bin/myterm"));
    QVERIFY(hasArg(launch, "/repo"));
    QVERIFY(hasArg(launch, "-e"));
}

void ExternalTerminalTest::konsoleGetsItsWorkdirFlag() {
    const TerminalLaunch launch = buildTerminalLaunch("/repo", "", resolverFor({"konsole"}));
    QVERIFY(launch.valid);
    QCOMPARE(QString::fromStdString(launch.program), QStringLiteral("/usr/bin/konsole"));
    QCOMPARE(launch.args, (std::vector<std::string>{"--workdir", "/repo"}));
}

void ExternalTerminalTest::gnomeTerminalUsesTheEqualsForm() {
    const TerminalLaunch launch =
        buildTerminalLaunch("/repo", "", resolverFor({"gnome-terminal"}));
    QVERIFY(launch.valid);
    QCOMPARE(launch.args, (std::vector<std::string>{"--working-directory=/repo"}));
}

void ExternalTerminalTest::xtermTakesTheGenericForm() {
    const TerminalLaunch launch = buildTerminalLaunch("/repo", "", resolverFor({"xterm"}));
    QVERIFY(launch.valid);
    QCOMPARE(QString::fromStdString(launch.program), QStringLiteral("/usr/bin/xterm"));
    QCOMPARE(launch.args.back(), std::string("/repo"));
    QVERIFY(hasArg(launch, "-e"));
}

void ExternalTerminalTest::anUnrecognisedTerminalTakesTheGenericForm() {
    const TerminalLaunch launch = buildTerminalLaunch("/repo", "/opt/wezterm", resolverFor({}));
    QVERIFY(launch.valid);
    QCOMPARE(QString::fromStdString(launch.program), QStringLiteral("/opt/wezterm"));
    QVERIFY(hasArg(launch, "-e"));
    QCOMPARE(launch.args.back(), std::string("/repo"));
}

void ExternalTerminalTest::noTerminalIsHonestlyInvalid() {
    const TerminalLaunch launch = buildTerminalLaunch("/repo", "", resolverFor({}));
    QVERIFY(!launch.valid);
    QVERIFY(!buildTerminalLaunch("", "", resolverFor({"konsole"})).valid);
}

QTEST_MAIN(ExternalTerminalTest)
#include "test_external_terminal.moc"
