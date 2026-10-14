#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

namespace {

/// On Windows QSettings lives in the registry, one store shared by every test
/// process at once, and XDG_CONFIG_HOME (how a test isolates it on Linux) means
/// nothing there. Each test process gets an INI store of its own, as the application
/// uses an INI file on Windows too.
void isolateSettings() {
    static QTemporaryDir store;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, store.path());
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(isolateSettings)
