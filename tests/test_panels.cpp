#include "model/commit_files_model.h"
#include "ui/commit_details_panel.h"
#include "ui/diff_view.h"
#include "ui/main_window.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

using namespace gk;

namespace {

ChangedFile changed(const QString& path, ChangeKind kind) {
    ChangedFile file;
    file.path = path.toStdString();
    file.kind = kind;
    return file;
}

CommitInfo commitInfo() {
    CommitInfo info;
    info.id = std::string(40, 'a');
    info.summary = "Polish the wording in config";
    info.author.name = "Demo Dev";
    info.author.email = "demo@example.com";
    info.author.whenUnix = 1759700000;
    info.author.offsetMinutes = 180;
    return info;
}

} // namespace

class PanelsTest : public QObject {
    Q_OBJECT

private slots:
    void selectingAFileDoesNotOpenTheDiff();
    void clickingAFileAsksForTheDiff();
    void theCloseButtonAsksForTheGraph();
    void aDirectoryRowIsNotAFile();
    void theDiffTakesTheGraphsPlaceAndTheRailGoesAway();
    void closingTheDiffBringsTheGraphAndTheRailBack();
    void aRailTheReaderHidStaysHidden();

    void theCommitFormIsUpForTheWorkingTree();
    void theCommitButtonWaitsForAMessage();
    void theCommitButtonCountsTheStagedFiles();
    void theSectionButtonsReportTheirSide();
    void hoveringAFileOffersToStageIt();
    void amendingRestoresTheDraftWhenUnticked();
    void theStashFormWaitsForATitle();
    void discardAllAsks();
    void theCoauthorButtonAsksForTheDialog();
    void theCommitMessageRoundTrips();
    void theSummaryIsHalfAgainTheDescription();
    void theCommitPanelHasAGripUnderTheMessageAndAnIconSort();
    void bothFacesShareOneMinimumWidth();
};

void PanelsTest::selectingAFileDoesNotOpenTheDiff() {
    CommitDetailsPanel panel;
    // The spies come first: the interesting thing happens inside showCommit(), and
    // a spy created afterwards records nothing.
    QSignalSpy selected(&panel, &CommitDetailsPanel::fileSelected);
    QSignalSpy activated(&panel, &CommitDetailsPanel::fileActivated);

    panel.showCommit(commitInfo(),
                     {changed(QStringLiteral("src/main.cpp"), ChangeKind::Modified)},
                     false);

    // What showCommit() does by itself is select the first file. That must render a
    // diff — and must not throw the graph off the screen under the reader.
    QVERIFY(selected.count() >= 1);
    QCOMPARE(activated.count(), 0);
    QVERIFY(!panel.firstFilePath().isEmpty());
}

void PanelsTest::clickingAFileAsksForTheDiff() {
    CommitDetailsPanel panel;
    panel.showCommit(commitInfo(),
                     {changed(QStringLiteral("README.md"), ChangeKind::Added)},
                     false);

    auto* view = panel.findChild<QAbstractItemView*>(QStringLiteral("commitFiles"));
    QVERIFY(view);
    const QModelIndex first = view->model()->index(0, 0);
    QVERIFY(first.isValid());

    QSignalSpy activated(&panel, &CommitDetailsPanel::fileActivated);
    // The click a reader makes: the row is already current, so only the click
    // handler can be what asks for the diff.
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                      view->visualRect(first).center());
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.first().first().toString(), QStringLiteral("README.md"));
}

void PanelsTest::theCloseButtonAsksForTheGraph() {
    // Through the window, not just through the widget: a signal that fires and a
    // button that works are two different claims, and only the second one matters
    // to someone clicking it.
    MainWindow window;
    window.showDiffPage();
    QVERIFY(window.diffPageVisible());

    // Searched inside the diff view itself: the window also holds the conflict
    // page, which has buttons of its own. Found by object name, not by glyph.
    auto* diffView = window.findChild<DiffView*>();
    QVERIFY(diffView);
    QToolButton* close = diffView->findChild<QToolButton*>(QStringLiteral("diffClose"));
    QVERIFY2(close, "the diff view must offer a way back to the graph");

    if (close) {
        close->click();
    }
    QVERIFY2(!window.diffPageVisible(), "✕ must put the graph back");
    QVERIFY(window.refsPanelVisible());
}

void PanelsTest::aDirectoryRowIsNotAFile() {
    CommitDetailsPanel panel;
    panel.showCommit(commitInfo(),
                     {changed(QStringLiteral("src/main.cpp"), ChangeKind::Modified),
                      changed(QStringLiteral("src/ui/window.cpp"), ChangeKind::Modified)},
                     false);
    panel.setMode(CommitFilesModel::TreeMode);

    // The first row of the tree is a folder, and the file the panel reports must be
    // a file: the diff cannot be asked for a directory.
    const QString path = panel.firstFilePath();
    QVERIFY(!path.isEmpty());
    QVERIFY(path.endsWith(QStringLiteral(".cpp")));
}

void PanelsTest::theDiffTakesTheGraphsPlaceAndTheRailGoesAway() {
    MainWindow window;
    QVERIFY(!window.diffPageVisible());
    QVERIFY(window.refsPanelVisible());

    window.showDiffPage();
    QVERIFY(window.diffPageVisible());
    // The rail goes with the graph: the room the diff gets is the point of putting
    // it in the middle column.
    QVERIFY(!window.refsPanelVisible());

    window.showGraphPage();
    QVERIFY(!window.diffPageVisible());
    QVERIFY(window.refsPanelVisible());
}

void PanelsTest::closingTheDiffBringsTheGraphAndTheRailBack() {
    MainWindow window;
    window.showDiffPage();
    QVERIFY(window.diffPageVisible());

    window.showGraphPage();
    QVERIFY(!window.diffPageVisible());
    QVERIFY(window.refsPanelVisible());
}

void PanelsTest::aRailTheReaderHidStaysHidden() {
    MainWindow window;
    window.setRefsPanelVisible(false);
    QVERIFY(!window.refsPanelVisible());

    // Opening and closing a diff must not undo a choice the reader made.
    window.showDiffPage();
    QVERIFY(!window.refsPanelVisible());
    window.showGraphPage();
    QVERIFY(!window.refsPanelVisible());
    QVERIFY(!window.diffPageVisible());
}

namespace {

QPushButton* buttonNamed(QWidget* root, const QString& text) {
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

FileChange workingChange(const QString& path, ChangeKind kind, bool staged) {
    FileChange change;
    change.path = path.toStdString();
    change.kind = kind;
    change.staged = staged;
    return change;
}

} // namespace

void PanelsTest::theCommitFormIsUpForTheWorkingTree() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({workingChange(QStringLiteral("src/main.cpp"), ChangeKind::Modified,
                                         /*staged=*/false),
                           workingChange(QStringLiteral("README.md"), ChangeKind::Modified,
                                         /*staged=*/true)});

    QVERIFY(panel.showingWorkingTree());
    auto* unstagedToggle = panel.findChild<QToolButton*>(QStringLiteral("unstagedToggle"));
    auto* stagedToggle = panel.findChild<QToolButton*>(QStringLiteral("stagedToggle"));
    QVERIFY(unstagedToggle && stagedToggle);
    QCOMPARE(unstagedToggle->text(), QStringLiteral("Unstaged Files (1)"));
    QCOMPARE(stagedToggle->text(), QStringLiteral("Staged Files (1)"));
    QVERIFY(buttonNamed(&panel, QStringLiteral("Stage All Changes")));
    QVERIFY(buttonNamed(&panel, QStringLiteral("Unstage All Changes")));
    QVERIFY(panel.findChild<QPushButton*>(QStringLiteral("discardAll")));
    auto* count = panel.findChild<QLabel*>(QStringLiteral("changeCount"));
    QVERIFY(count);
    QCOMPARE(count->text(), QStringLiteral("2 file changes on"));
    auto* tabs = panel.findChild<QTabWidget*>(QStringLiteral("commitTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->tabText(0), QStringLiteral("Commit"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("Stash"));
    // The unstaged list comes first, so its file is the one picked for the diff.
    QCOMPARE(panel.firstFilePath(), QStringLiteral("src/main.cpp"));
    QVERIFY(!panel.firstFileIsStaged());
    QCOMPARE(panel.selectedPaths(), QStringList{QStringLiteral("src/main.cpp")});
}

void PanelsTest::theCommitButtonWaitsForAMessage() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({workingChange(QStringLiteral("a.txt"), ChangeKind::Added, true)});

    auto* summary = panel.findChild<QLineEdit*>(QStringLiteral("commitSummary"));
    auto* description = panel.findChild<QPlainTextEdit*>(QStringLiteral("commitDescription"));
    auto* commit = panel.findChild<QPushButton*>(QStringLiteral("commitButton"));
    QVERIFY(summary && description && commit);
    QVERIFY2(!commit->isEnabled(), "an empty message is not a commit");
    QCOMPARE(commit->text(), QStringLiteral("Type a Message to Commit"));

    // A description alone is not a message: the summary is the line git shows.
    description->setPlainText(QStringLiteral("Only a body"));
    QVERIFY(!commit->isEnabled());

    summary->setText(QStringLiteral("Do the thing"));
    QVERIFY(commit->isEnabled());
    QCOMPARE(commit->text(), QStringLiteral("Commit Changes to 1 File"));

    QSignalSpy requested(&panel, &CommitDetailsPanel::commitRequested);
    commit->click();
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.first().first().toString(), QStringLiteral("Do the thing\n\nOnly a body"));
}

void PanelsTest::theCommitButtonCountsTheStagedFiles() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({workingChange(QStringLiteral("a.txt"), ChangeKind::Modified, false)});
    auto* summary = panel.findChild<QLineEdit*>(QStringLiteral("commitSummary"));
    auto* commit = panel.findChild<QPushButton*>(QStringLiteral("commitButton"));
    QVERIFY(summary && commit);
    summary->setText(QStringLiteral("Message"));
    QVERIFY(!commit->isEnabled());

    panel.showWorkingTree({workingChange(QStringLiteral("a.txt"), ChangeKind::Modified, true),
                           workingChange(QStringLiteral("b.txt"), ChangeKind::Added, true),
                           workingChange(QStringLiteral("c.txt"), ChangeKind::Added, false)});
    QVERIFY(commit->isEnabled());
    QCOMPARE(commit->text(), QStringLiteral("Commit Changes to 2 Files"));
}

void PanelsTest::theSectionButtonsReportTheirSide() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({workingChange(QStringLiteral("a.txt"), ChangeKind::Modified, false),
                           workingChange(QStringLiteral("b.txt"), ChangeKind::Added, true)});

    QSignalSpy stageAll(&panel, &CommitDetailsPanel::stageAllRequested);
    QSignalSpy unstageAll(&panel, &CommitDetailsPanel::unstageAllRequested);
    buttonNamed(&panel, QStringLiteral("Stage All Changes"))->click();
    buttonNamed(&panel, QStringLiteral("Unstage All Changes"))->click();
    QCOMPARE(stageAll.count(), 1);
    QCOMPARE(unstageAll.count(), 1);
    QCOMPARE(panel.stagedPaths(), QStringList{QStringLiteral("b.txt")});

    // Folding one list gives the other the whole height.
    auto* toggle = panel.findChild<QToolButton*>(QStringLiteral("unstagedToggle"));
    auto* unstaged = panel.findChild<QTreeView*>(QStringLiteral("unstagedFiles"));
    QVERIFY(toggle && unstaged);
    QVERIFY(!unstaged->isHidden());
    toggle->click();
    QVERIFY(unstaged->isHidden());
}

void PanelsTest::hoveringAFileOffersToStageIt() {
    CommitDetailsPanel panel;
    panel.resize(420, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.showWorkingTree({workingChange(QStringLiteral("a.txt"), ChangeKind::Modified, false),
                           workingChange(QStringLiteral("b.txt"), ChangeKind::Added, true)});

    QSignalSpy staged(&panel, &CommitDetailsPanel::stageRequested);
    QSignalSpy unstaged(&panel, &CommitDetailsPanel::unstageRequested);
    QSignalSpy activated(&panel, &CommitDetailsPanel::fileActivated);

    // The button sits at the right end of the row; a click on it acts on that file
    // and does not open its diff.
    const auto clickButton = [](QTreeView* view) {
        const QModelIndex row = view->model()->index(0, 0);
        const QRect rect = view->visualRect(row);
        const QPoint target(view->viewport()->width() - 12, rect.center().y());
        QTest::mouseMove(view->viewport(), target);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, target);
    };
    clickButton(panel.findChild<QTreeView*>(QStringLiteral("unstagedFiles")));
    clickButton(panel.findChild<QTreeView*>(QStringLiteral("stagedFiles")));
    QTRY_COMPARE(staged.count(), 1);
    QTRY_COMPARE(unstaged.count(), 1);
    QCOMPARE(staged.first().first().toStringList(), QStringList{QStringLiteral("a.txt")});
    QCOMPARE(unstaged.first().first().toStringList(), QStringList{QStringLiteral("b.txt")});
    QCOMPARE(activated.count(), 0);
}

void PanelsTest::amendingRestoresTheDraftWhenUnticked() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({workingChange(QStringLiteral("a.txt"), ChangeKind::Modified, false)});
    panel.setCommitMessage(QStringLiteral("My draft"));

    auto* amend = panel.findChild<QCheckBox*>(QStringLiteral("amendCommit"));
    auto* commit = panel.findChild<QPushButton*>(QStringLiteral("commitButton"));
    QVERIFY(amend && commit);
    // The window answers the tick with HEAD's message.
    connect(&panel, &CommitDetailsPanel::amendToggled, &panel, [&panel](bool on) {
        if (on) {
            panel.setCommitMessage(QStringLiteral("Previous subject\n\nPrevious body"));
        }
    });
    amend->setChecked(true);
    QVERIFY(panel.amendRequested());
    QCOMPARE(panel.commitMessage(), QStringLiteral("Previous subject\n\nPrevious body"));
    // An amend can reword with nothing staged.
    QVERIFY(commit->isEnabled());
    QCOMPARE(commit->text(), QStringLiteral("Amend Previous Commit"));

    amend->setChecked(false);
    QCOMPARE(panel.commitMessage(), QStringLiteral("My draft"));

    amend->setChecked(true);
    panel.clearCommitMessage(); // what the window does after a commit
    QVERIFY(!panel.amendRequested());
    QVERIFY(panel.commitMessage().isEmpty());
}

void PanelsTest::theStashFormWaitsForATitle() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({workingChange(QStringLiteral("a.txt"), ChangeKind::Modified, false),
                           workingChange(QStringLiteral("a.txt"), ChangeKind::Modified, true),
                           workingChange(QStringLiteral("b.txt"), ChangeKind::Untracked, false)});
    auto* title = panel.findChild<QLineEdit*>(QStringLiteral("stashTitle"));
    auto* description = panel.findChild<QPlainTextEdit*>(QStringLiteral("stashDescription"));
    auto* stash = panel.findChild<QPushButton*>(QStringLiteral("stashButton"));
    QVERIFY(title && description && stash);
    QVERIFY(!stash->isEnabled());
    QCOMPARE(stash->text(), QStringLiteral("Type a Message to Stash"));

    title->setText(QStringLiteral("WIP login"));
    description->setPlainText(QStringLiteral("half done"));
    QVERIFY(stash->isEnabled());
    // A path changed on both sides is one file to stash.
    QCOMPARE(stash->text(), QStringLiteral("Stash Changes to 2 Files"));

    QSignalSpy requested(&panel, &CommitDetailsPanel::stashAllRequested);
    stash->click();
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.first().first().toString(), QStringLiteral("WIP login\n\nhalf done"));
    panel.clearStashMessage();
    QVERIFY(title->text().isEmpty());
}

void PanelsTest::discardAllAsks() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({workingChange(QStringLiteral("a.txt"), ChangeKind::Modified, false)});
    QSignalSpy requested(&panel, &CommitDetailsPanel::discardAllRequested);
    auto* discard = panel.findChild<QPushButton*>(QStringLiteral("discardAll"));
    QVERIFY(discard);
    QVERIFY(discard->isEnabled());
    discard->click();
    QCOMPARE(requested.count(), 1);

    panel.showWorkingTree({});
    QVERIFY(!discard->isEnabled());
}

void PanelsTest::theCoauthorButtonAsksForTheDialog() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({});
    QSignalSpy requested(&panel, &CommitDetailsPanel::coauthorsRequested);
    QPushButton* button = buttonNamed(&panel, QStringLiteral("Co-authors…"));
    QVERIFY(button);
    // It lives under "Commit options", folded away until the reader opens them.
    auto* options = panel.findChild<QToolButton*>(QStringLiteral("commitOptions"));
    QVERIFY(options);
    QVERIFY(button->parentWidget()->isHidden());
    options->click();
    QVERIFY(!button->parentWidget()->isHidden());
    button->click();
    QCOMPARE(requested.count(), 1);
}

void PanelsTest::theCommitMessageRoundTrips() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({});
    // The window reads the field to build a trailer block and writes it back, so the
    // panel has to hold exactly what was set — no trimming, no reformatting.
    panel.setCommitMessage(QStringLiteral("Subject\n\nBody"));
    QCOMPARE(panel.commitMessage(), QStringLiteral("Subject\n\nBody"));
    panel.clearCommitMessage();
    QVERIFY(panel.commitMessage().isEmpty());
}

void PanelsTest::theSummaryIsHalfAgainTheDescription() {
    CommitDetailsPanel panel;
    panel.showWorkingTree({workingChange(QStringLiteral("a.txt"), ChangeKind::Modified, false)});
    panel.resize(460, 800);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));

    auto* summary = panel.findChild<QLineEdit*>(QStringLiteral("commitSummary"));
    auto* description = panel.findChild<QPlainTextEdit*>(QStringLiteral("commitDescription"));
    auto* stashTitle = panel.findChild<QLineEdit*>(QStringLiteral("stashTitle"));
    QVERIFY(summary);
    QVERIFY(description);
    QVERIFY(stashTitle);
    // One page is off the stack and one tab is not selected, and a stylesheet's font is
    // only resolved on polish.
    summary->ensurePolished();
    description->ensurePolished();
    stashTitle->ensurePolished();

    const int body = description->font().pixelSize();
    QVERIFY2(body > 0, qPrintable(QString::number(body)));
    // Half again, not twice: the summary is the first line of the message rather than a
    // heading over it. The stash title is the same field on the other tab, so the two
    // cannot drift apart.
    QCOMPARE(summary->font().pixelSize(), qRound(body * 1.5));
    QCOMPARE(stashTitle->font().pixelSize(), qRound(body * 1.5));
}

void PanelsTest::theCommitPanelHasAGripUnderTheMessageAndAnIconSort() {
    CommitDetailsPanel panel;
    panel.showCommit(commitInfo(), {changed(QStringLiteral("a.txt"), ChangeKind::Modified)}, true);

    // The sort is a button with an icon, not a dropdown. A menu listed three entries
    // for one piece of state the reader cannot see, and it looked nothing like the
    // Commit Panel's own sort control one page away in the same stack.
    auto* sort = panel.findChild<QToolButton*>(QStringLiteral("commitSort"));
    QVERIFY(sort);
    QVERIFY2(!sort->menu(), "the file-list sort has to be a button, not a dropdown");
    QVERIFY(!sort->icon().isNull());
    QVERIFY(!sort->toolTip().isEmpty());
    const qint64 before = sort->icon().cacheKey();
    sort->click();
    QVERIFY2(sort->icon().cacheKey() != before,
             "clicking the sort button has to move it to the next order");

    // The message and everything under it are the splitter's two children, so the page
    // has exactly one handle and it sits between the message and the author: nothing
    // is draggable between the author block and the files.
    auto* split = panel.findChild<QSplitter*>(QStringLiteral("commitDetailsSplit"));
    QVERIFY(split);
    QCOMPARE(split->count(), 2);
    auto* messageArea = panel.findChild<QWidget*>(QStringLiteral("commitMessageArea"));
    auto* metaArea = panel.findChild<QWidget*>(QStringLiteral("commitMetaArea"));
    auto* message = panel.findChild<QPlainTextEdit*>(QStringLiteral("commitMessage"));
    QVERIFY(messageArea);
    QVERIFY(metaArea);
    QVERIFY(message);
    QCOMPARE(split->widget(0), messageArea);
    QCOMPARE(metaArea->parentWidget(), split->widget(1));
    QCOMPARE(message->parentWidget(), messageArea);

    panel.resize(460, 800);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QList<int> sizes = split->sizes();
    const int was = sizes.at(0);
    sizes[0] = was + 60;
    sizes[1] = qMax(40, sizes.at(1) - 60);
    split->setSizes(sizes);
    QVERIFY2(message->height() > was, "the handle has to give the message more room");
}

void PanelsTest::bothFacesShareOneMinimumWidth() {
    CommitDetailsPanel panel;
    panel.showCommit(commitInfo(),
                     {changed(QStringLiteral("src/main.cpp"), ChangeKind::Modified)}, false);
    const int forCommit = panel.minimumSizeHint().width();
    panel.showWorkingTree({workingChange(QStringLiteral("src/a-rather-long-file-name.cpp"),
                                         ChangeKind::Modified, false),
                           workingChange(QStringLiteral("b.txt"), ChangeKind::Added, true)});
    const int forWorkingTree = panel.minimumSizeHint().width();
    // The side panel is one splitter pane: switching what it shows must not change
    // how narrow the reader can drag it.
    QCOMPARE(forWorkingTree, forCommit);

    // And the shared minimum is the commit face's own: the Commit Panel fits inside
    // it rather than widening the panel for both.
    auto* pages = panel.findChild<QStackedWidget*>();
    QVERIFY(pages && pages->count() == 2);
    QVERIFY2(pages->widget(1)->minimumSizeHint().width() <=
                 pages->widget(0)->minimumSizeHint().width(),
             qPrintable(QStringLiteral("commit %1, working tree %2")
                            .arg(pages->widget(0)->minimumSizeHint().width())
                            .arg(pages->widget(1)->minimumSizeHint().width())));
}

QTEST_MAIN(PanelsTest)
#include "test_panels.moc"
