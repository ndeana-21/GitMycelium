#pragma once

#include "ui/recent_repositories.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace gk {

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
