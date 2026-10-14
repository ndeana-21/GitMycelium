#pragma once

#include "core/types.h"

#include <QDialog>
#include <QString>

#include <vector>

class QListWidget;

namespace gk {

class FileHistoryDialog : public QDialog {
    Q_OBJECT

public:
    FileHistoryDialog(const QString& path, const std::vector<FileHistoryEntry>& entries,
                      QWidget* parent = nullptr);

    QString selectedCommit() const;

private:
    QListWidget* m_list = nullptr;
};

} // namespace gk
