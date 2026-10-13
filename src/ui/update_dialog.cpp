#include "ui/update_dialog.h"

#include "update/updater.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {
namespace {

const QUrl kReleasesPage(QStringLiteral("https://github.com/ndeana-21/GitMycelium/releases/latest"));

} // namespace

UpdateDialog::UpdateDialog(update::Updater* updater, QWidget* parent)
    : QDialog(parent), m_updater(updater) {
    setWindowTitle(QStringLiteral("Check for Updates"));
    setMinimumWidth(460);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("updateStatus"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_install = buttons->addButton(QStringLiteral("Install and Restart"), QDialogButtonBox::ActionRole);
    m_install->setObjectName(QStringLiteral("updateInstall"));
    m_check = buttons->addButton(QStringLiteral("Check Again"), QDialogButtonBox::ActionRole);
    m_releasePage = buttons->addButton(QStringLiteral("Release Page"), QDialogButtonBox::HelpRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_install, &QPushButton::clicked, this, [this] {
        m_error.clear();
        m_progress->setRange(0, 0);
        m_progress->setVisible(true);
        m_updater->install();
        showState();
    });
    connect(m_check, &QPushButton::clicked, this, [this] {
        m_error.clear();
        m_updater->check();
        showState();
    });
    connect(m_releasePage, &QPushButton::clicked, this, [this] {
        const auto& manifest = m_updater->manifest();
        QDesktopServices::openUrl(manifest && manifest->notesUrl.isValid() ? manifest->notesUrl
                                                                            : kReleasesPage);
    });

    connect(m_updater, &update::Updater::checked, this, &UpdateDialog::onChecked);
    connect(m_updater, &update::Updater::installFinished, this, &UpdateDialog::onInstallFinished);
    connect(m_updater, &update::Updater::progress, this, [this](qint64 received, qint64 total) {
        if (total > 0 && received < total) {
            m_progress->setRange(0, 1000);
            m_progress->setValue(static_cast<int>(received * 1000 / total));
            return;
        }
        // Downloaded: what follows (checksum, unpacking, signature) has no measure.
        m_progress->setRange(0, 0);
        m_installing = true;
        showState();
    });

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addStretch(1);
    layout->addWidget(buttons);

    if (!m_updater->manifest() && !m_updater->busy()) {
        m_updater->check();
    }
    showState();
}

void UpdateDialog::showState() {
    using namespace update;
    const QString installed =
        QStringLiteral("Installed: <b>%1</b>")
            .arg(describeVersion(currentVersion(), currentBuild()).toHtmlEscaped());

    QString text;
    if (m_updater->busy()) {
        if (m_installing) {
            text = QStringLiteral("Verifying and installing the update…");
        } else if (m_updater->manifest() && m_progress->isVisible()) {
            text = QStringLiteral("Downloading the update…");
        } else {
            text = QStringLiteral("Checking for updates…");
        }
    } else if (!m_error.isEmpty()) {
        text = m_error.toHtmlEscaped();
    } else if (!m_updater->manifest()) {
        text = QString();
    } else if (!m_updater->updateAvailable()) {
        text = QStringLiteral("This is the newest build.");
    } else {
        const UpdateManifest& manifest = *m_updater->manifest();
        text = QStringLiteral("Available: <b>%1</b>")
                   .arg(describeVersion(manifest.version, manifest.build).toHtmlEscaped());
        const QString refusal = m_updater->installRefusal();
        if (!refusal.isEmpty()) {
            text += QStringLiteral("<br><br>") + refusal.toHtmlEscaped();
        }
    }
    m_status->setText(text.isEmpty() ? installed : installed + QStringLiteral("<br>") + text);

    m_install->setEnabled(!m_updater->busy() && m_updater->installRefusal().isEmpty());
    m_check->setEnabled(!m_updater->busy());
}

void UpdateDialog::onChecked(bool ok, const QString& error) {
    m_error = ok ? QString() : error;
    showState();
}

void UpdateDialog::onInstallFinished(bool ok, const QString& error) {
    m_progress->setVisible(false);
    m_installing = false;
    if (!ok) {
        m_error = error;
        showState();
        return;
    }
    if (m_updater->restart()) {
        emit restartRequested();
        accept();
        return;
    }
    m_error = QStringLiteral("The update is installed; restart GitMycelium to use it.");
    showState();
}

} // namespace gk
