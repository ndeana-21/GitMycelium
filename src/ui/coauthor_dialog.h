#pragma once

#include "core/types.h"

#include <QDialog>
#include <QString>

#include <vector>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace gk {

class CoAuthorDialog : public QDialog {
    Q_OBJECT

public:
    explicit CoAuthorDialog(QWidget* parent = nullptr);

    void setSuggestions(const std::vector<CoAuthor>& authors);
    std::vector<CoAuthor> authors() const;

private:
    void addCurrent();
    void refreshOkState();

    QLineEdit* m_name = nullptr;
    QLineEdit* m_email = nullptr;
    QComboBox* m_recent = nullptr;
    QPushButton* m_add = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_ok = nullptr;
    std::vector<CoAuthor> m_suggestions;
    std::vector<CoAuthor> m_authors;
};

} // namespace gk
