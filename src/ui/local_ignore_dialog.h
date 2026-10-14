#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QListWidget;

namespace gk {

/// What this checkout hides from the reader alone, without touching the shared
/// `.gitignore`: the patterns in `.git/info/exclude` and the tracked files marked
/// skip-worktree. The dialog only reports the reader's edits; the window writes them.
class LocalIgnoreDialog : public QDialog {
    Q_OBJECT

public:
    LocalIgnoreDialog(const QStringList& patterns, const QStringList& hiddenFiles,
                      QWidget* parent = nullptr);

    QStringList addedPatterns() const;
    QStringList removedPatterns() const;
    /// Hidden tracked files the reader brought back.
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
