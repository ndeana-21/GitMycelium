#pragma once

#include "core/types.h"

#include <QDialog>
#include <QString>

#include <vector>

class QListWidget;

namespace gk {

/// The commits that touched one file, newest first, as `git log --follow` reports them.
/// A rename is followed, so the list holds the commits made under the file's earlier
/// names too. Choosing a row hands the commit back to the window, which selects it in
/// the graph — the dialog knows nothing about the repository.
class FileHistoryDialog : public QDialog {
    Q_OBJECT

public:
    FileHistoryDialog(const QString& path, const std::vector<FileHistoryEntry>& entries,
                      QWidget* parent = nullptr);

    /// The commit of the chosen row, or empty when nothing was chosen.
    QString selectedCommit() const;

private:
    QListWidget* m_list = nullptr;
};

} // namespace gk
