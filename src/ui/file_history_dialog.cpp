#include "ui/file_history_dialog.h"

#include "theme/theme.h"

#include <QDateTime>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {

FileHistoryDialog::FileHistoryDialog(const QString& path,
                                     const std::vector<FileHistoryEntry>& entries,
                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("File History — %1").arg(path));
    setModal(true);
    resize(640, 420);

    auto* layout = new QVBoxLayout(this);
    auto* header = new QLabel(QStringLiteral("%1 commit(s) touched %2").arg(entries.size()).arg(path),
                              this);
    header->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    layout->addWidget(header);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);
    for (const FileHistoryEntry& entry : entries) {
        const QString when = QDateTime::fromSecsSinceEpoch(entry.whenUnix)
                                 .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  %2  %3").arg(QString::fromStdString(entry.id).left(8).leftJustified(8),
                                            when, QString::fromStdString(entry.author).left(14)),
            m_list);
        item->setData(Qt::UserRole, QString::fromStdString(entry.id));
        item->setToolTip(QString::fromStdString(entry.summary));
        item->setText(item->text() + QStringLiteral("  ") + QString::fromStdString(entry.summary));
    }
    layout->addWidget(m_list, 1);

    auto* buttons = new QVBoxLayout();
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    // Double-click a commit to jump to it; a single click only highlights.
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] { accept(); });
}

QString FileHistoryDialog::selectedCommit() const {
    if (!m_list || !m_list->currentItem()) {
        return {};
    }
    return m_list->currentItem()->data(Qt::UserRole).toString();
}

} // namespace gk
