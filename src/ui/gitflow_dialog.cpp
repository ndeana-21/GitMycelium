#include "ui/gitflow_dialog.h"

#include "theme/theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace gk {

GitFlowDialog::GitFlowDialog(bool initialized, const QString& master, const QString& develop,
                             QWidget* parent)
    : QDialog(parent), m_initialized(initialized) {
    setWindowTitle(initialized ? QStringLiteral("Gitflow") : QStringLiteral("Initialize Gitflow"));
    setMinimumSize(480, 400);
    setStyleSheet(theme::styleSheet());

    auto* root = new QVBoxLayout(this);
    auto* state = new QLabel(
        initialized ? QStringLiteral("Gitflow is configured: master “%1”, develop “%2”.")
                          .arg(master, develop)
                    : QStringLiteral("Gitflow is not configured for this repository yet."),
        this);
    state->setWordWrap(true);
    state->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim().name()));
    root->addWidget(state);

    auto* form = new QFormLayout;
    m_master = new QLineEdit(master, this);
    m_develop = new QLineEdit(develop, this);
    form->addRow(QStringLiteral("Master branch"), m_master);
    form->addRow(QStringLiteral("Develop branch"), m_develop);
    root->addLayout(form);

    auto* initButton = new QPushButton(QStringLiteral("Initialize"), this);
    initButton->setEnabled(!initialized);
    root->addWidget(initButton);

    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    root->addWidget(separator);

    auto* topicForm = new QFormLayout;
    m_kind = new QComboBox(this);
    m_kind->addItems({QStringLiteral("feature"), QStringLiteral("hotfix"), QStringLiteral("release"),
                      QStringLiteral("bugfix")});
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("my-change"));
    m_version = new QLineEdit(this);
    m_version->setPlaceholderText(QStringLiteral("only for release/hotfix, e.g. 1.2.0"));
    topicForm->addRow(QStringLiteral("Kind"), m_kind);
    topicForm->addRow(QStringLiteral("Name"), m_name);
    topicForm->addRow(QStringLiteral("Version"), m_version);
    root->addLayout(topicForm);

    auto* buttons = new QHBoxLayout;
    m_startButton = new QPushButton(QStringLiteral("Start"), this);
    m_finishButton = new QPushButton(QStringLiteral("Finish"), this);
    m_startButton->setEnabled(initialized);
    m_finishButton->setEnabled(initialized);
    buttons->addWidget(m_startButton);
    buttons->addWidget(m_finishButton);
    buttons->addStretch(1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->addWidget(close);
    root->addLayout(buttons);

    connect(initButton, &QPushButton::clicked, this, [this] {
        m_action = Action::Initialize;
        accept();
    });
    connect(m_startButton, &QPushButton::clicked, this, [this] {
        if (m_name->text().trimmed().isEmpty()) {
            m_name->setFocus();
            return;
        }
        m_action = Action::Start;
        accept();
    });
    connect(m_finishButton, &QPushButton::clicked, this, [this] {
        if (m_name->text().trimmed().isEmpty()) {
            m_name->setFocus();
            return;
        }
        m_action = Action::Finish;
        accept();
    });
}

QString GitFlowDialog::masterBranch() const {
    return m_master->text().trimmed();
}

QString GitFlowDialog::developBranch() const {
    return m_develop->text().trimmed();
}

QString GitFlowDialog::kind() const {
    return m_kind->currentText();
}

QString GitFlowDialog::name() const {
    return m_name->text().trimmed();
}

QString GitFlowDialog::version() const {
    return m_version->text().trimmed();
}

} // namespace gk
