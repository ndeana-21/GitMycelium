#include "ui/start_page.h"

#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace gk {

StartPage::StartPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(48, 40, 48, 40);
    layout->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("GitMycelium"), this);
    title->setObjectName(QStringLiteral("startPageTitle"));
    QFont titleFont = title->font();
    // The application font is set in pixels (`theme::applyApplicationTheme`), so
    // `pointSizeF()` is -1 here and doubling it asked Qt for -2 — a warning, and a
    // title left at the base size. Double the unit the font actually uses.
    if (titleFont.pixelSize() > 0) {
        titleFont.setPixelSize(titleFont.pixelSize() * 2);
    } else if (titleFont.pointSizeF() > 0) {
        titleFont.setPointSizeF(titleFont.pointSizeF() * 2.0);
    }
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto* subtitle =
        new QLabel(QStringLiteral("Open a repository, clone one, or create a new one."), this);
    subtitle->setObjectName(QStringLiteral("startPageSubtitle"));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_open = new QPushButton(QStringLiteral("Open Repository…"), this);
    m_open->setObjectName(QStringLiteral("startPageOpen"));
    m_open->setProperty("variant", QStringLiteral("primary"));
    m_clone = new QPushButton(QStringLiteral("Clone Repository…"), this);
    m_clone->setObjectName(QStringLiteral("startPageClone"));
    m_create = new QPushButton(QStringLiteral("Create Repository…"), this);
    m_create->setObjectName(QStringLiteral("startPageCreate"));
    buttons->addWidget(m_open);
    buttons->addWidget(m_clone);
    buttons->addWidget(m_create);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* recentLabel = new QLabel(QStringLiteral("Recent repositories"), this);
    recentLabel->setObjectName(QStringLiteral("startPageRecentLabel"));
    QFont sectionFont = recentLabel->font();
    sectionFont.setBold(true);
    recentLabel->setFont(sectionFont);
    layout->addSpacing(10);
    layout->addWidget(recentLabel);

    m_recentEmpty = new QLabel(QStringLiteral("Nothing opened yet."), this);
    m_recentEmpty->setObjectName(QStringLiteral("startPageRecentEmpty"));
    layout->addWidget(m_recentEmpty);

    m_recent = new QTreeWidget(this);
    m_recent->setObjectName(QStringLiteral("recentList"));
    m_recent->setColumnCount(3);
    m_recent->setHeaderLabels(
        {QStringLiteral("Repository"), QStringLiteral("Path"), QStringLiteral("Last used")});
    m_recent->setRootIsDecorated(false);
    m_recent->setUniformRowHeights(true);
    m_recent->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recent->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recent->setColumnWidth(0, 220);
    m_recent->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(m_recent, 1);

    connect(m_open, &QPushButton::clicked, this, &StartPage::openRequested);
    connect(m_clone, &QPushButton::clicked, this, &StartPage::cloneRequested);
    connect(m_create, &QPushButton::clicked, this, &StartPage::createRequested);
    connect(m_recent, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item) {
            emit recentActivated(item->data(0, Qt::UserRole).toString());
        }
    });
    connect(m_recent, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (item) {
            emit recentActivated(item->data(0, Qt::UserRole).toString());
        }
    });

    setRecentRepositories({});
}

void StartPage::setRecentRepositories(const QVector<RecentRepository>& entries) {
    m_recent->clear();
    for (const RecentRepository& entry : entries) {
        auto* item = new QTreeWidgetItem(m_recent);
        item->setText(0, entry.name.isEmpty() ? entry.path : entry.name);
        item->setText(1, entry.path);
        item->setText(2, entry.lastUsed.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        item->setData(0, Qt::UserRole, entry.path);
        item->setToolTip(1, entry.path);
    }
    const bool empty = entries.isEmpty();
    m_recentEmpty->setVisible(empty);
    m_recent->setVisible(!empty);
}

} // namespace gk
