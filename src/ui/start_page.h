#pragma once

#include "ui/recent_repositories.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace gk {

/// What a tab shows when it holds no repository: the way in, and the way back.
///
/// Three buttons — open an existing repository, clone one, create one — and the
/// list of repositories used recently, each with its path and when it was last
/// opened. The page holds no session: it reports what the reader asked for and the
/// window performs it, because the same actions are reachable from the menu.
class StartPage : public QWidget {
    Q_OBJECT

public:
    explicit StartPage(QWidget* parent = nullptr);

    void setRecentRepositories(const QVector<RecentRepository>& entries);

signals:
    void openRequested();
    void cloneRequested();
    void createRequested();
    void recentActivated(const QString& path);

private:
    QPushButton* m_open = nullptr;
    QPushButton* m_clone = nullptr;
    QPushButton* m_create = nullptr;
    QLabel* m_recentEmpty = nullptr;
    QTreeWidget* m_recent = nullptr;
};

} // namespace gk
