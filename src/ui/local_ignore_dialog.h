#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QListWidget;

namespace gk {

class LocalIgnoreDialog : public QDialog {
    Q_OBJECT

public:
    LocalIgnoreDialog(const QStringList& patterns, const QStringList& hiddenFiles,
                      QWidget* parent = nullptr);

    QStringList addedPatterns() const;
    QStringList removedPatterns() const;
    QStringList unhiddenFiles() const;

    /// Test seams.
    void addPattern(const QString& pattern);
    void removePattern(const QString& pattern);
    void unhideFile(const QString& path);

private:
    static QStringList itemsOf(const QListWidget* list);

    QStringList m_patterns;
    QStringList m_hidden;
    QListWidget* m_patternList = nullptr;
    QListWidget* m_hiddenList = nullptr;
    QLineEdit* m_newPattern = nullptr;
};

} // namespace gk
