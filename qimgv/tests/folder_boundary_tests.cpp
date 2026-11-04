#include "engine_stack_fixture.h"

// the configured folder-end behaviour and the agreement between the folder view's
// order and the navigation's
class TestFolderBoundary : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestFolderBoundary() : EngineStackFixture("folder-boundary") {}

private slots:
    void initTestCase() { init(); }

    // "After reaching the end of a folder" is a setting with three values, so what
    // gets pinned here is the behavior each value produces — not the absence of one
    // old bug. decide() takes the mode by value, so these cases inject the value and
    // never touch the settings singleton; whether the singleton hands out that value
    // at all is its own one assertion below.

    void folderEndActionSettingRoundTripsAndClamps() {
        // The boundary cases above take the mode by value; this is the one thing
        // the settings layer adds: the key stores all three values and a junk
        // value in a hand-edited config file falls back to "no action" instead of
        // indexing past the enum.
        const QList<FolderEndAction> valid = {FOLDER_END_NO_ACTION, FOLDER_END_LOOP, FOLDER_END_GOTO_ADJACENT};
        for(const FolderEndAction mode : valid) {
            settings->setFolderEndAction(mode);
            QVERIFY(settings->folderEndAction() == mode);
        }
        settings->setFolderEndAction(static_cast<FolderEndAction>(7));
        QVERIFY(settings->folderEndAction() == FOLDER_END_NO_ACTION);
        settings->setFolderEndAction(static_cast<FolderEndAction>(-1));
        QVERIFY(settings->folderEndAction() == FOLDER_END_NO_ACTION);
        settings->setFolderEndAction(FOLDER_END_NO_ACTION); // leave the default
    }

    void noActionReportsTheBoundaryAndStaysPut() {
        const FolderEndAction mode = FOLDER_END_NO_ACTION;

        // off the end (forward) and off the start (backward): report, do not move
        QVERIFY(FolderEnd::decide(mode, 3, 3).outcome == FolderEnd::Outcome::ReportBoundary);
        QVERIFY(FolderEnd::decide(mode, -1, 3).outcome == FolderEnd::Outcome::ReportBoundary);
        // a step that stays inside the folder still steps
        auto inside = FolderEnd::decide(mode, 1, 3);
        QVERIFY(inside.outcome == FolderEnd::Outcome::LoadIndex);
        QCOMPARE(inside.index, 1);
    }

    void loopActionWrapsToTheFarEndOfTheSameFolder() {
        const FolderEndAction mode = FOLDER_END_LOOP;

        auto offEnd = FolderEnd::decide(mode, 3, 3);
        QVERIFY(offEnd.outcome == FolderEnd::Outcome::LoadIndex);
        QCOMPARE(offEnd.index, 0);
        auto beforeStart = FolderEnd::decide(mode, -1, 3);
        QVERIFY(beforeStart.outcome == FolderEnd::Outcome::LoadIndex);
        QCOMPARE(beforeStart.index, 2);
    }

    void adjacentActionStepsOutOfTheFolderAtEitherEdge() {
        const FolderEndAction mode = FOLDER_END_GOTO_ADJACENT;

        QVERIFY(FolderEnd::decide(mode, 3, 3).outcome == FolderEnd::Outcome::EnterAdjacentFolder);
        QVERIFY(FolderEnd::decide(mode, -1, 3).outcome == FolderEnd::Outcome::EnterAdjacentFolder);
        // in the middle of the folder it behaves like every other mode
        QVERIFY(FolderEnd::decide(mode, 1, 3).outcome == FolderEnd::Outcome::LoadIndex);
    }

    void adjacentActionEntersTheNextFolderInParentOrder() {
        // The promise the setting makes: at the last image of alpha you go to beta.
        // adjacentDirectory() is what Core calls, so this covers the resolution
        // itself — routed through the async listing it returns empty here and the
        // feature is dead again.
        resetFixture();
        QVERIFY(QDir(fixtureDir).mkdir("alpha"));
        QVERIFY(QDir(fixtureDir).mkdir("beta"));
        QVERIFY(QDir(fixtureDir).mkdir("zeta"));

        const QString alpha = fixtureDir + "/alpha";
        const QString beta = fixtureDir + "/beta";
        const QString sub = fixtureDir + "/subdir";
        const QString zeta = fixtureDir + "/zeta";

        // natural name order: alpha, beta, subdir, zeta
        QCOMPARE(DirectoryManager::adjacentDirectory(alpha, true, SORT_NAME), beta);
        QCOMPARE(DirectoryManager::adjacentDirectory(beta, true, SORT_NAME), sub);
        QCOMPARE(DirectoryManager::adjacentDirectory(sub, true, SORT_NAME), zeta);
        QCOMPARE(DirectoryManager::adjacentDirectory(zeta, false, SORT_NAME), sub);
        QCOMPARE(DirectoryManager::adjacentDirectory(alpha, false, SORT_NAME), QString());
        // the last folder has nowhere to go — that is what makes Core show its
        // end-of-folder message instead of jumping
        QCOMPARE(DirectoryManager::adjacentDirectory(zeta, true, SORT_NAME), QString());
    }

    void folderBoundaryLookupKeepsWatchingTheCurrentFolder() {
        // A boundary that has nowhere to go must leave the user's own folder
        // watched: the lookup is not allowed to take the engine off it, or live
        // updates die for the rest of the session with nothing on screen to show
        resetFixture();

        DirectoryManager dm;
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        auto decision = FolderEnd::decide(FOLDER_END_GOTO_ADJACENT, static_cast<int>(dm.fileCount()),
                                          dm.fileCount());
        QVERIFY(decision.outcome == FolderEnd::Outcome::EnterAdjacentFolder);
        // the transition this keypress would take: nowhere — subdir is the only and
        // therefore the last folder here
        QVERIFY(DirectoryManager::adjacentDirectory(fixtureDir + "/subdir", true, SORT_NAME).isEmpty());

        QSignalSpy added(&dm, &DirectoryManager::fileAdded);
        makeFile("after-boundary.jpg");
        QTRY_COMPARE_WITH_TIMEOUT(added.count(), 1, 10000);
        QCOMPARE(dm.fileCount(), 4UL);
    }

    // The view's sibling order is the engine's (scan.rs gives every directory the
    // same size/mtime, so its comparator lands on the name tiebreak) and the mirror
    // keeps that order. The sibling lookup sorts locally, so under a TIME or SIZE
    // mode the two were free to disagree: std::sort over an all-equivalent range is
    // not stable, so "the next folder" could be a different one than the next row.

    void folderNavigationWalksSiblingsInTheOrderTheViewShows() {
        resetFixture();
        // created in reverse name order on purpose, so a degenerate local sort
        // cannot land on the expected order by accident
        QVERIFY(QDir(fixtureDir).mkdir("zulu"));
        QVERIFY(QDir(fixtureDir).mkdir("mike"));
        QVERIFY(QDir(fixtureDir).mkdir("bravo"));
        QStringList names = {fixtureDir + "/bravo", fixtureDir + "/mike",
                             fixtureDir + "/subdir", fixtureDir + "/zulu"};
        QStringList reversed;
        for (int i = names.size() - 1; i >= 0; --i)
            reversed << names.at(i);

        DirectoryManager dm;
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.dirCount(), 4UL, 10000);

        const SortingMode modes[] = {SORT_NAME, SORT_NAME_DESC, SORT_TIME, SORT_TIME_DESC,
                                     SORT_SIZE, SORT_SIZE_DESC};
        for(SortingMode mode : modes) {
            dm.setSortingMode(mode);
            QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);
            // the rows are the check, not the mode: sortingMode() reports the
            // mode this instance *asked* for, which is written before the
            // engine is told and so was true the moment the call returned
            QStringList view;
            for(unsigned long i = 0; i < dm.dirCount(); ++i)
                view << dm.dirPathAt(int(i));

            // walking the siblings must reproduce the rows, in the same direction
            QStringList walked;
            QString cur = dm.dirPathAt(0);
            while(!cur.isEmpty() && walked.size() <= names.size()) {
                walked << cur;
                cur = DirectoryManager::adjacentDirectory(cur, true, mode);
            }

            const QStringList &want = (mode == SORT_NAME_DESC) ? reversed : names;
            QCOMPARE(view, want);
            QCOMPARE(walked, want);
        }
    }

    void newlyCreatedFoldersKeepTheirPlaceInNameOrder() {
        // The other half of the same rule: a folder arriving through the watcher is
        // *inserted*, not re-sorted. The insert used to share the file comparator,
        // and since every directory carries the same size/mtime (scan.rs sets them,
        // and onEngineMutated zeroes modifyTime for dirs) a time or size comparator
        // saw all entries as equal and appended the newcomer at the end of the rows.
        resetFixture();
        QVERIFY(QDir(fixtureDir).mkdir("alpha"));
        QVERIFY(QDir(fixtureDir).mkdir("zulu"));

        DirectoryManager dm;
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.dirCount(), 3UL, 10000); // alpha, subdir, zulu

        dm.setSortingMode(SORT_TIME);
        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);

        // a name that belongs in the middle, never at the end
        QVERIFY(QDir(fixtureDir).mkdir("mike"));
        QTRY_COMPARE_WITH_TIMEOUT(dm.dirCount(), 4UL, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);

        QStringList rows;
        for(unsigned long i = 0; i < dm.dirCount(); ++i)
            rows << dm.dirNameAt(int(i));
        const QStringList want = {QStringLiteral("alpha"), QStringLiteral("mike"),
                                  QStringLiteral("subdir"), QStringLiteral("zulu")};
        QCOMPARE(rows, want);
        // navigation walks the same order
        QCOMPARE(DirectoryManager::adjacentDirectory(fixtureDir + "/mike", true, SORT_TIME),
                 fixtureDir + "/subdir");
    }

    void settlePredicateTracksTheReplyInsteadOfTheRequest() {
        // isSettled() is only worth waiting on if it is false while the engine
        // still owes a reply; otherwise every QTRY_VERIFY(isSettled()) in this
        // file is a sleep with a better name. The reverse order is used because
        // it always changes the listing, whatever the persisted mode was.
        resetFixture();

        DirectoryManager dm;
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);
        QVERIFY2(dm.fileCount() > 1, "a one-row resort proves nothing");

        const SortingMode flip = (dm.sortingMode() == SORT_NAME) ? SORT_NAME_DESC : SORT_NAME;
        dm.setSortingMode(flip);
        // synchronous window: the requested mode is already stored (this is what
        // the old wait tested) while the reply can only arrive via the event loop
        QVERIFY(dm.sortingMode() == flip);
        QVERIFY(!dm.isSettled());

        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);
    }
};

#include "folder_boundary_tests.moc"

int runFolderBoundaryTests(int argc, char **argv) {
    TestFolderBoundary test;
    return runIfOwned(test, argc, argv);
}
