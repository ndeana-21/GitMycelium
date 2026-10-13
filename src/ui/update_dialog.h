#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

namespace gk {

namespace update {
class Updater;
}

/// Help → Check for Updates: what is installed, what is published, and one button
/// that downloads, verifies and puts the new build in place.
class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    explicit UpdateDialog(update::Updater* updater, QWidget* parent = nullptr);

signals:
    /// The update is in place; the window closes so the relaunch can start it.
    void restartRequested();

private:
    void showState();
    void onChecked(bool ok, const QString& error);
    void onInstallFinished(bool ok, const QString& error);

    update::Updater* m_updater;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QPushButton* m_install = nullptr;
    QPushButton* m_check = nullptr;
    QPushButton* m_releasePage = nullptr;
    QString m_error;
    bool m_installing = false;
};

} // namespace gk
