#include "ui/graph_operation_panel.h"

#include "theme/theme.h"
#include "ui/badge_flight.h"

#include <QCloseEvent>
#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace gk {

QString describeMergePreview(const MergePreview& preview) {
    if (!preview.available) {
        return QStringLiteral("Nothing to predict — already up to date");
    }
    if (preview.clean) {
        return QStringLiteral("No conflicts predicted");
    }
    QStringList names;
    for (const std::string& path : preview.conflictedPaths) {
        names << QString::fromStdString(path);
    }
    constexpr int kMaxListed = 4;
    QString listed = names.mid(0, kMaxListed).join(QStringLiteral(", "));
    if (names.size() > kMaxListed) {
        listed += QStringLiteral(", +%1 more").arg(names.size() - kMaxListed);
    }
    if (names.isEmpty()) {
        listed = QStringLiteral("(git named no files)");
    }
    return QStringLiteral("Conflicts in %1 file(s): %2").arg(names.size()).arg(listed);
}

GraphOperationPanel::GraphOperationPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("graphOperationPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    m_title = new QLabel(this);
    m_title->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:600;")
                               .arg(theme::textDim().name()));
    layout->addWidget(m_title);

    // The prediction sits with the choice: a reader should not learn about a conflict
    // only after picking Merge.
    m_preview = new QLabel(this);
    m_preview->setWordWrap(true);
    m_preview->setMaximumWidth(280);
    layout->addWidget(m_preview);

    auto addEntry = [this, layout](const QString& text, bool enabled) {
        auto* button = new QPushButton(text, this);
        button->setEnabled(enabled);
        button->setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
        layout->addWidget(button);
        return button;
    };

    m_rebase = addEntry(QString(), true);
    m_merge = addEntry(QString(), true);
    m_squash = addEntry(QString(), true);
    m_interactive = addEntry(QString(), true);
    m_checkout = addEntry(QString(), true);

    applyTheme();

    connect(m_rebase, &QPushButton::clicked, this, [this] { report(RefOperation::Rebase); });
    connect(m_merge, &QPushButton::clicked, this, [this] { report(RefOperation::Merge); });
    connect(m_squash, &QPushButton::clicked, this, [this] { report(RefOperation::Squash); });
    connect(m_interactive, &QPushButton::clicked, this,
            [this] { report(RefOperation::InteractiveRebase); });
    connect(m_checkout, &QPushButton::clicked, this, [this] { report(RefOperation::Checkout); });

    hide();
}

void GraphOperationPanel::applyTheme() {
    m_title->setStyleSheet(QStringLiteral("color:%1; font-size:%2px; font-weight:600;")
                               .arg(theme::textDim().name())
                               .arg(std::max(9, theme::baseFontPixelSize() - 2)));
    const QString entry =
        QStringLiteral("QPushButton { text-align:left; padding:%1px %2px; border:none; color:%3; "
                       "background:transparent; font-size:%4px; }"
                       "QPushButton:hover:enabled { background:%5; color:%6; }"
                       "QPushButton:disabled { color:%7; }")
            .arg(theme::scaledPixels(4))
            .arg(theme::scaledPixels(8))
            .arg(theme::text().name())
            .arg(std::max(9, theme::baseFontPixelSize() - 1))
            .arg(theme::selection().name())
            .arg(theme::text().name())
            .arg(theme::textDim().name());
    for (QPushButton* button : {m_rebase, m_merge, m_squash, m_interactive, m_checkout}) {
        if (button) {
            button->setStyleSheet(entry);
        }
    }
    setStyleSheet(
        QStringLiteral("#graphOperationPanel { background:%1; border:1px solid %2; "
                       "border-radius:%3px; }")
            .arg(theme::surface().name(), theme::border().name())
            .arg(theme::radius()));
}

void GraphOperationPanel::present(const QString& source, const QString& target, const QPoint& at,
                                  const QRect& room, const QString& preview, bool previewWarns) {
    applyTheme();
    m_title->setText(QStringLiteral("%1 → %2").arg(source, target));
    m_rebase->setText(QStringLiteral("Rebase %1 onto %2").arg(source, target));
    // The operation merges the dragged branch *into* the one it was dropped on, which
    // is what the handler does; the label has to say the same thing.
    m_merge->setText(QStringLiteral("Merge %1 into %2").arg(source, target));
    m_squash->setText(QStringLiteral("Squash %1 into %2").arg(source, target));
    m_interactive->setText(
        QStringLiteral("Interactive rebase %1 onto %2").arg(source, target));
    m_checkout->setText(QStringLiteral("Check out %1").arg(target));
    m_preview->setText(preview);
    m_preview->setVisible(!preview.isEmpty());
    m_preview->setStyleSheet(QStringLiteral("color:%1; font-size:%2px;")
                                 .arg((previewWarns ? theme::warning() : theme::textDim()).name())
                                 .arg(std::max(9, theme::baseFontPixelSize() - 2)));
    m_reported = false;

    // Size first, place second: clamping before the labels were set would press the
    // list back against the edge by the wrong amount.
    adjustSize();
    QPoint where = at;
    if (room.isValid()) {
        where.setX(qBound(room.left(), where.x(), std::max(room.left(), room.right() - width())));
        where.setY(qBound(room.top(), where.y(), std::max(room.top(), room.bottom() - height())));
    }
    move(where);
    // Watching the application, not just this widget: the press that dismisses the list
    // is usually one that never reaches it.
    qApp->installEventFilter(this);
    show();
    raise();
    setFocus(Qt::OtherFocusReason);
    logBadgeFlow(QStringLiteral("panel: shown at %1,%2 size %3x%4 for %5 onto %6")
                     .arg(x())
                     .arg(y())
                     .arg(width())
                     .arg(height())
                     .arg(source, target));
}

void GraphOperationPanel::report(RefOperation operation) {
    if (m_reported) {
        return;
    }
    qApp->removeEventFilter(this);
    m_reported = true;
    logBadgeFlow(QStringLiteral("panel: entry clicked — %1").arg(static_cast<int>(operation)));
    hide();
    emit chosen(operation);
}

void GraphOperationPanel::dismiss(const QString& reason) {
    if (m_reported) {
        return;
    }
    m_reported = true;
    qApp->removeEventFilter(this);
    logBadgeFlow(QStringLiteral("panel: dismissed (%1)").arg(reason));
    hide();
    emit dismissed();
}

bool GraphOperationPanel::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched);
    if (!isVisible()) {
        return false;
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto* press = static_cast<QMouseEvent*>(event);
        const QPoint global = press->globalPosition().toPoint();
        if (!rect().contains(mapFromGlobal(global))) {
            dismiss(QStringLiteral("press outside, at %1,%2")
                        .arg(global.x())
                        .arg(global.y()));
        }
        return false;
    }

    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            dismiss(QStringLiteral("escape"));
        }
        return false;
    }

    return false;
}

void GraphOperationPanel::closeEvent(QCloseEvent* event) {
    // A closed list is a dismissal, and every dismissal has to be reported: the badge
    // is waiting on it.
    dismiss();
    QWidget::closeEvent(event);
}

void GraphOperationPanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        dismiss();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool GraphOperationPanel::event(QEvent* event) {
    return QWidget::event(event);
}

} // namespace gk
