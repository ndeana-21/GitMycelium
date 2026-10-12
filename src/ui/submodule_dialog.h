#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

namespace gk {

/// Asks for the three things `git submodule add` needs: the repository URL, the
/// path it lands at, and — optionally — the branch it should follow. The last one
/// is what `submodule update --remote` reads, and leaving it empty is the usual
/// answer, which is why it is a plain field and not a picker.
class SubmoduleDialog : public QDialog {
    Q_OBJECT

public:
    explicit SubmoduleDialog(QWidget* parent = nullptr);

    QString url() const;
    /// Empty lets git derive it from the URL.
    QString path() const;
    /// `submodule.<name>.branch`; empty leaves it unset.
    QString branch() const;

private:
    QLineEdit* m_url = nullptr;
    QLineEdit* m_path = nullptr;
    QLineEdit* m_branch = nullptr;
};

} // namespace gk
