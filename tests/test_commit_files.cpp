#include "model/commit_files_model.h"

#include <QStandardItemModel>
#include <QtTest>

using namespace gk;

namespace {

ChangedFile file(const QString& path, ChangeKind kind = ChangeKind::Modified) {
    ChangedFile out;
    out.path = path.toStdString();
    out.kind = kind;
    return out;
}

/// "parent|label" for every row, so a test can state the shape it expects in one
/// literal rather than chasing indexes.
void collect(const QStandardItemModel& model, const QModelIndex& parent, const QString& trail,
             QStringList* out) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        const QString label = index.data(Qt::DisplayRole).toString();
        const QString path = trail.isEmpty() ? label : trail + QLatin1Char('/') + label;
        out->push_back(path);
        collect(model, index, path, out);
    }
}

QStringList shape(const CommitFilesModel& model) {
    QStringList out;
    collect(model, QModelIndex(), QString(), &out);
    return out;
}

} // namespace

class CommitFilesTest : public QObject {
    Q_OBJECT

private slots:
    void pathModeListsEveryFileInOrder();
    void treeModeFoldsFolders();
    void singleChildFoldersMerge();
    void descendingReversesTheOrder();
    void statusSortGroupsByKind();
    void countsPerKind();
    void folderFilterNarrowsTheList();
    void workingTreeGroupsByStaged();
};

void CommitFilesTest::pathModeListsEveryFileInOrder() {
    CommitFilesModel model;
    model.setFiles({file(QStringLiteral("src/main.cpp")), file(QStringLiteral("README.md")),
                    file(QStringLiteral("src/ui/window.cpp"))});

    const QStringList rows = shape(model);
    QCOMPARE(rows, QStringList({QStringLiteral("README.md"), QStringLiteral("src/main.cpp"),
                                QStringLiteral("src/ui/window.cpp")}));
}

void CommitFilesTest::treeModeFoldsFolders() {
    CommitFilesModel model;
    model.setFiles({file(QStringLiteral("src/main.cpp")), file(QStringLiteral("src/ui/window.cpp")),
                    file(QStringLiteral("README.md"))});
    model.setMode(CommitFilesModel::TreeMode);

    const QStringList rows = shape(model);
    QCOMPARE(rows, QStringList({QStringLiteral("src"), QStringLiteral("src/ui"),
                                QStringLiteral("src/ui/window.cpp"),
                                QStringLiteral("src/main.cpp"), QStringLiteral("README.md")}));

    QVERIFY(!rows.contains(QStringLiteral("src/src/main.cpp")));
}

void CommitFilesTest::singleChildFoldersMerge() {
    CommitFilesModel model;
    model.setFiles({file(QStringLiteral("src/models/user.cpp"))});
    model.setMode(CommitFilesModel::TreeMode);

    QCOMPARE(shape(model),
             QStringList({QStringLiteral("src/models"), QStringLiteral("src/models/user.cpp")}));
}

void CommitFilesTest::descendingReversesTheOrder() {
    CommitFilesModel model;
    model.setFiles({file(QStringLiteral("a.cpp")), file(QStringLiteral("z.cpp"))});
    model.setSort(CommitFilesModel::PathDescending);

    QCOMPARE(shape(model), QStringList({QStringLiteral("z.cpp"), QStringLiteral("a.cpp")}));
}

void CommitFilesTest::statusSortGroupsByKind() {
    CommitFilesModel model;
    model.setFiles({file(QStringLiteral("z-modified.cpp"), ChangeKind::Modified),
                    file(QStringLiteral("a-added.cpp"), ChangeKind::Added),
                    file(QStringLiteral("m-deleted.cpp"), ChangeKind::Deleted)});
    model.setSort(CommitFilesModel::ByStatus);

    QCOMPARE(shape(model), QStringList({QStringLiteral("a-added.cpp"),
                                        QStringLiteral("z-modified.cpp"),
                                        QStringLiteral("m-deleted.cpp")}));
}

void CommitFilesTest::countsPerKind() {
    CommitFilesModel model;
    model.setFiles({file(QStringLiteral("one.cpp"), ChangeKind::Added),
                    file(QStringLiteral("two.cpp"), ChangeKind::Added),
                    file(QStringLiteral("three.cpp"), ChangeKind::Modified),
                    file(QStringLiteral("four.cpp"), ChangeKind::Deleted)});

    QCOMPARE(model.countOf(ChangeKind::Added), 2);
    QCOMPARE(model.countOf(ChangeKind::Modified), 1);
    QCOMPARE(model.countOf(ChangeKind::Deleted), 1);
    QCOMPARE(model.countOf(ChangeKind::Renamed), 0);
    QCOMPARE(model.fileCount(), 4);
}

void CommitFilesTest::folderFilterNarrowsTheList() {
    CommitFilesModel model;
    model.setFiles({file(QStringLiteral("src/main.cpp")), file(QStringLiteral("src/ui/window.cpp")),
                    file(QStringLiteral("docs/readme.md"))});

    model.setFolderFilter(QStringLiteral("src/ui/"));
    QCOMPARE(shape(model), QStringList({QStringLiteral("src/ui/window.cpp")}));

    // ...and the same filter narrows the tree shape, without a second code path.
    model.setMode(CommitFilesModel::TreeMode);
    QCOMPARE(shape(model),
             QStringList({QStringLiteral("src/ui"), QStringLiteral("src/ui/window.cpp")}));

    model.setFolderFilter(QString());
    QCOMPARE(model.fileCount(), 3);
    QVERIFY(shape(model).contains(QStringLiteral("src")));
}

void CommitFilesTest::workingTreeGroupsByStaged() {
    CommitFilesModel model;
    FileChange staged;
    staged.path = "staged.cpp";
    staged.kind = ChangeKind::Modified;
    staged.staged = true;
    FileChange unstaged;
    unstaged.path = "unstaged.cpp";
    unstaged.kind = ChangeKind::Added;
    model.setWorkingTree({staged, unstaged});

    const QStringList rows = shape(model);
    QCOMPARE(rows, QStringList({QStringLiteral("Staged (1)"), QStringLiteral("Staged (1)/staged.cpp"),
                                QStringLiteral("Unstaged (1)"),
                                QStringLiteral("Unstaged (1)/unstaged.cpp")}));
    QVERIFY(model.workingTree());
}

QTEST_MAIN(CommitFilesTest)
#include "test_commit_files.moc"
