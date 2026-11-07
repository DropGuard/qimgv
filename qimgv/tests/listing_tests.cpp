#include "engine_stack_fixture.h"

#include <QDeadlineTimer> // the convergence deadline below is a wall-clock budget
#include <cstdio> // std::rename: RENAME_NOREPLACE is QFile::rename, and an
                  // overwrite is exactly what the paired event is for

// the bridge event pump, DirectoryManager's async open, the deferred-listing
// primitive and the single-writer rule
class TestListing : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestListing() : EngineStackFixture("listing") {}

private slots:
    void initTestCase() { init(); }

    void engineBridgePumpDeliversListed() {
        resetFixture();
        auto &bridge = EngineBridge::instance();
        QSignalSpy listed(&bridge, &EngineBridge::dirListed);
        QVERIFY(listed.isValid());

        QVERIFY(bridge.openDir(fixtureDir, 0 /*QIMGV_SORT_NAME*/, false));
        QTRY_COMPARE_WITH_TIMEOUT(listed.count(), 1, 10000);
        // the event, not a count, is what says this listing is ours
        QCOMPARE(bridge.listedDirectory(), fixtureDir);

        QCOMPARE(bridge.fileCount(), 3); // txt filtered
        QCOMPARE(bridge.dirCount(), 1);
        EngineBridge::EngineEntry e;
        QVERIFY(bridge.entryAt(0, false, e));
        QCOMPARE(e.name, QStringLiteral("img1.jpg")); // natural order: 1 < 2 < 10
        QVERIFY(bridge.entryAt(2, false, e));
        QCOMPARE(e.name, QStringLiteral("img10.jpg"));
    }

    void directoryManagerOpenIsAsynchronous() {
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QSignalSpy loaded(&dm, &DirectoryManager::loaded);
        QSignalSpy failed(&dm, &DirectoryManager::loadingFailed);

        QVERIFY(dm.setDirectory(fixtureDir));
        // THE async assertion: listing must NOT be there yet (old code was sync)
        QCOMPARE(dm.fileCount(), 0UL);
        QCOMPARE(loaded.count(), 0);

        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QCOMPARE(dm.fileCount(), 3UL);
        QCOMPARE(dm.dirCount(), 1UL);
        QCOMPARE(dm.filePathAt(0), fixtureDir + "/img1.jpg");
        QCOMPARE(dm.filePathAt(2), fixtureDir + "/img10.jpg");
        QCOMPARE(failed.count(), 0);
    }

    void missingDirectoryFailsFast() {
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QVERIFY(!dm.setDirectory("/nonexistent/qimgv-test-42"));
    }

    void sortingResortsAsynchronously() {
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        QSignalSpy resort(&dm, &DirectoryManager::sortingChanged);
        dm.setSortingMode(SORT_NAME_DESC);
        QTRY_COMPARE_WITH_TIMEOUT(resort.count(), 1, 10000);
        QCOMPARE(dm.filePathAt(0), fixtureDir + "/img10.jpg");
        QCOMPARE(dm.filePathAt(2), fixtureDir + "/img1.jpg");
    }

    void liveFileCreationArrivesViaWatcher() {
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QSignalSpy added(&dm, &DirectoryManager::fileAdded);
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        // a file appears while the directory is open; on Linux the engine
        // watcher applies it incrementally (no rescan involved)
        makeFile("latecomer.jpg");
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 4UL, 10000);
        QCOMPARE(added.count(), 1);
        QCOMPARE(added.at(0).at(0).toString(), fixtureDir + "/latecomer.jpg");
        // the engine listing is authoritative: natural order kept
        QCOMPARE(dm.filePathAt(0), fixtureDir + "/img1.jpg");
        QCOMPARE(dm.filePathAt(3), fixtureDir + "/latecomer.jpg");
    }

    void liveFileDeletionEmitsRemoved() {
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        QVERIFY(QFile::remove(fixtureDir + "/img2.jpg"));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 2UL, 10000);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(dm.filePathAt(0), fixtureDir + "/img1.jpg");
        QCOMPARE(dm.filePathAt(1), fixtureDir + "/img10.jpg");
    }

    void liveFileModificationKeepsTheMirrorEntryWhole() {
        // the engine watcher's "modified" event is applied by re-statting the
        // file through FSEntry's path constructor. That constructor once
        // produced an empty entry for every path: the mirror replaced a valid
        // entry with an empty one, the fileModified signal still fired,
        // nothing crashed — the file just silently vanished from the mirror.
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QSignalSpy modified(&dm, &DirectoryManager::fileModified);
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        QFile f(fixtureDir + "/img2.jpg");
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Append));
        QVERIFY(f.write("appended") == 8);
        f.close();

        QTRY_VERIFY_WITH_TIMEOUT(modified.count() >= 1, 10000);
        bool sawImg2Modified = false;
        for(int i = 0; i < modified.count(); ++i)
            sawImg2Modified |= modified.at(i).at(0).toString() == fixtureDir + "/img2.jpg";
        QVERIFY(sawImg2Modified);

        // the entry survives the round trip whole: re-stat, not emptied
        int index = dm.indexOfFile(fixtureDir + "/img2.jpg");
        QVERIFY(index >= 0);
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileEntryAt(index).size, 9UL, 10000);
        QCOMPARE(dm.fileEntryAt(index).name, QStringLiteral("img2.jpg"));
        QCOMPARE(dm.fileEntryAt(index).path, fixtureDir + "/img2.jpg");
        QVERIFY(!dm.fileEntryAt(index).isDirectory);
    }

    void liveRenameIsOneEvent() {
        // The watcher pairs the kernel's MOVED_FROM/MOVED_TO cookie, so a rename
        // reaches the mirror as ONE event moving ONE row: no window in which the
        // file is missing from the listing, and no remove+add churn that costs
        // the view a widget — and the picture inside it — for a file whose bytes
        // never changed.
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
        QSignalSpy added(&dm, &DirectoryManager::fileAdded);
        QSignalSpy renamed(&dm, &DirectoryManager::fileRenamed);
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);
        const int oldIndex = dm.indexOfFile(fixtureDir + "/img2.jpg");
        QVERIFY(oldIndex >= 0);

        QVERIFY(QFile::rename(fixtureDir + "/img2.jpg", fixtureDir + "/renamed.jpg"));
        QTRY_COMPARE_WITH_TIMEOUT(renamed.count(), 1, 10000);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(added.count(), 0);
        QCOMPARE(renamed.at(0).at(0).toString(), fixtureDir + "/img2.jpg");
        QCOMPARE(renamed.at(0).at(1).toString(), fixtureDir + "/renamed.jpg");
        QCOMPARE(renamed.at(0).at(2).toInt(), oldIndex);

        // one event, and the row it moved is the engine's own row: name sort
        // lands "renamed" after "img1" and "img10", so the entry repositioned —
        // the rename is a move under this mode, not an in-place edit
        QCOMPARE(dm.fileCount(), 3UL);
        const int newIndex = dm.indexOfFile(fixtureDir + "/renamed.jpg");
        QVERIFY(newIndex != oldIndex);
        QVERIFY(!dm.containsFile(fixtureDir + "/img2.jpg"));
        // the entry rode along whole: size is the stat the engine already had,
        // not a re-derive and not a zeroed placeholder
        QCOMPARE(dm.fileEntryAt(newIndex).name, QStringLiteral("renamed.jpg"));
        QCOMPARE(dm.fileEntryAt(newIndex).size, 1UL);
        assertMirrorMatchesEngineExport(dm);
    }

    void overwriteRenameReplacesTheListedRow() {
        // rename(2) onto a path that is already listed. The kernel sends
        // MOVED_FROM + MOVED_TO all the same, and until the pair was answered
        // this was invisible for good: MOVED_TO on a listed path was
        // Ignored(AlreadyListed), so the destination row kept the size, mtime
        // and picture of the file that used to live there.
        resetFixture();
        {
            QFile f(fixtureDir + "/over.jpg");
            QVERIFY(f.open(QIODevice::WriteOnly));
            QCOMPARE(f.write(QByteArray(7, 'x')), 7);
        }
        DirectoryManager dm{EngineBridge::instance()};
        QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
        QSignalSpy added(&dm, &DirectoryManager::fileAdded);
        QSignalSpy renamed(&dm, &DirectoryManager::fileRenamed);
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 4UL, 10000);
        const int source = dm.indexOfFile(fixtureDir + "/over.jpg");
        const int dest = dm.indexOfFile(fixtureDir + "/img1.jpg");
        QVERIFY(source >= 0 && dest >= 0);

        // QFile::rename would not do: it maps to RENAME_NOREPLACE and cannot
        // overwrite, so it cannot even produce the event this test is about.
        const QByteArray from = (fixtureDir + "/over.jpg").toLocal8Bit();
        const QByteArray to = (fixtureDir + "/img1.jpg").toLocal8Bit();
        QCOMPARE(std::rename(from.constData(), to.constData()), 0);

        QTRY_COMPARE_WITH_TIMEOUT(renamed.count(), 1, 10000);
        QCOMPARE(added.count(), 0);
        QCOMPARE(renamed.at(0).at(0).toString(), fixtureDir + "/over.jpg");
        QCOMPARE(renamed.at(0).at(1).toString(), fixtureDir + "/img1.jpg");
        // the row the overwrite took away is announced before the move, because
        // every index after it shifts — including the one just passed to this
        // signal, which is the position the view is holding when it fires
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(0).toString(), fixtureDir + "/img1.jpg");
        QCOMPARE(renamed.at(0).at(2).toInt(), (source > dest) ? source - 1 : source);
        // four rows became three: an overwrite consumes the destination entry,
        // which is the whole difference between this and a plain rename
        QCOMPARE(dm.fileCount(), 3UL);
        QVERIFY(!dm.containsFile(fixtureDir + "/over.jpg"));
        // and the surviving row now describes the file that is actually there
        QCOMPARE(dm.fileEntryAt(dm.indexOfFile(fixtureDir + "/img1.jpg")).size, 7UL);
        assertMirrorMatchesEngineExport(dm);
    }

    // "Decide when the listing lands" used to be a hand-rolled flag in Core
    // with three clear/consume sites; whenListed() centralizes the lifecycle,
    // and these tests pin it.

    void whenListedFiresWhenTheListingLands() {
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QVERIFY(dm.setDirectory(fixtureDir));
        bool fired = false;
        dm.whenListed(fixtureDir, [&fired] { fired = true; });
        QVERIFY(!fired); // armed, the listing is still in flight
        QTRY_VERIFY_WITH_TIMEOUT(fired, 10000);
    }

    void whenListedIsSupersededByANewerOpen() {
        resetFixture();
        QVERIFY(QDir(fixtureDir).mkdir("second"));
        QFile f(fixtureDir + "/second/z.jpg");
        QVERIFY(f.open(QIODevice::WriteOnly));

        DirectoryManager dm{EngineBridge::instance()};
        QVERIFY(dm.setDirectory(fixtureDir));
        bool staleFired = false;
        dm.whenListed(fixtureDir, [&staleFired] { staleFired = true; });
        // a newer open supersedes the queue before the stale listing lands
        QVERIFY(dm.setDirectory(fixtureDir + "/second"));
        QVERIFY(!staleFired);
        bool freshFired = false;
        dm.whenListed(fixtureDir + "/second", [&freshFired] { freshFired = true; });
        QTRY_VERIFY_WITH_TIMEOUT(freshFired, 10000);
        QVERIFY(!staleFired);
    }

    void whenListedCanBeCancelled() {
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QVERIFY(dm.setDirectory(fixtureDir));
        bool fired = false;
        qint64 token = dm.whenListed(fixtureDir, [&fired] { fired = true; });
        QVERIFY(dm.cancelListed(token));
        // the listing lands (the counts arrive), the cancelled action stays
        // silent, and a double cancel is a no-op rather than an error
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3, 10000);
        // the pending actions run inside the same listing event, so a settled
        // mirror is the bound; the sleep only hoped the event had been processed
        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);
        QVERIFY(!fired);
        QVERIFY(!dm.cancelListed(token));
    }

    // In-app file operations touch the disk only; the mirror has exactly one
    // writer (the engine event applier) and the update arrives through the
    // watch round trip, the same path an external edit takes.

    void modelFileOperationsRideTheEngineRoundTrip() {
        resetFixture();
        WorkerPool pool(2);
        DirectoryModel model(pool);
        QSignalSpy removed(&model, &DirectoryModel::fileRemoved);
        QSignalSpy added(&model, &DirectoryModel::fileAdded);
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 3UL, 10000);

        FileOpResult result;
        model.removeFile(fixtureDir + "/img2.jpg", false, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        // the removal signal carries the update when the round trip lands,
        // not synchronously from the operation
        QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 10000);
        QCOMPARE(removed.at(0).at(0).toString(), fixtureDir + "/img2.jpg");
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 2UL, 10000);

        // rename in place surfaces as remove+add, exactly like an external one
        model.renameEntry(fixtureDir + "/img1.jpg", "renamed.jpg", false, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QTRY_VERIFY_WITH_TIMEOUT(model.containsFile(fixtureDir + "/renamed.jpg"), 10000);
        QVERIFY(!model.containsFile(fixtureDir + "/img1.jpg"));

        // copy into the watched directory — engine-driven from the start
        QVERIFY(QFile::copy(fixtureDir + "/renamed.jpg", fixtureDir + "/copy.jpg"));
        QTRY_VERIFY_WITH_TIMEOUT(model.containsFile(fixtureDir + "/copy.jpg"), 10000);
        QCOMPARE(model.fileCount(), 2UL + 1UL);
    }

    // the contract check itself: the mirror vectors against the engine's export,
    // order included — same entries, same positions. Tells whether they agree
    // and, when they do not, names the disagreement: a count that differs says
    // "the mirror is wrong", which is not actionable, while the two lists say
    // which entry the mirror never heard about.
    static bool mirrorMatchesEngineExport(DirectoryManager &dm, QString &diff) {
        QByteArray arena;
        quint32 fileCount = 0, dirCount = 0;
        if(!EngineBridge::instance().exportListing(arena, fileCount, dirCount)) {
            diff = QStringLiteral("exportListing failed");
            return false;
        }
        std::vector<EngineBridge::EngineEntry> files, dirs;
        if(!EngineBridge::parseListing(arena, files, dirs)) {
            diff = QStringLiteral("parseListing rejected a %1-byte arena").arg(arena.size());
            return false;
        }
        auto joinNames = [](const std::vector<EngineBridge::EngineEntry> &entries) {
            QStringList out;
            for(const auto &e : entries)
                out << e.name;
            return out.join(QLatin1Char(' '));
        };
        auto joinMirrorFileNames = [&dm]() {
            QStringList out;
            for(unsigned long i = 0; i < dm.fileCount(); ++i)
                out << dm.fileNameAt(int(i));
            return out.join(QLatin1Char(' '));
        };
        auto fail = [&](const QString &what) {
            diff = QStringLiteral("%1 — engine: %2 files [%3] / mirror: %4 files [%5]")
                       .arg(what).arg(int(files.size()))
                       .arg(joinNames(files)).arg(int(dm.fileCount())).arg(joinMirrorFileNames());
            return false;
        };
        if(files.size() != dm.fileCount())
            return fail(QStringLiteral("file count"));
        if(dirs.size() != dm.dirCount())
            return fail(QStringLiteral("dir count"));
        for(size_t i = 0; i < files.size(); ++i) {
            if(dm.filePathAt(static_cast<int>(i)) != files[i].path)
                return fail(QStringLiteral("file row %1").arg(int(i)));
        }
        for(size_t i = 0; i < dirs.size(); ++i) {
            if(dm.dirPathAt(static_cast<int>(i)) != dirs[i].path)
                return fail(QStringLiteral("dir row %1").arg(int(i)));
        }
        return true;
    }

    // Convergence is the moment the contract speaks about. The pump that moves an
    // event from the engine's queue into these vectors runs on the Qt event loop,
    // and isSettled() says nothing about that queue — it reports the mirrored
    // directory, the applied sort and the absence of a resync — so a pull taken
    // the instant it turns true can compare the engine's now against the mirror's
    // then. Waiting for agreement is the only way to name the point where the
    // contract holds; a mirror that never agrees still fails, after the deadline,
    // with both listings in the message.
    void assertMirrorMatchesEngineExport(DirectoryManager &dm) {
        QString diff;
        bool agrees = mirrorMatchesEngineExport(dm, diff);
        QDeadlineTimer deadline(10000);
        while(!agrees && !deadline.hasExpired()) {
            QTest::qWait(20);
            agrees = mirrorMatchesEngineExport(dm, diff);
        }
        QVERIFY2(agrees, qPrintable(diff));
    }

    // THE mirror contract: the engine decides content and order; the mirror is
    // a faithful projection of it. After any mutation sequence the vectors
    // must equal the engine's export without this side re-deriving either.
    // (The pre-contract applier sorted incrementally with a lexicographic path
    // compare, so "img3.jpg" landed somewhere else than the engine's natural
    // order put it.)
    void mirrorStaysAFaithfulProjectionOfTheEngineListing() {
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        // tricky names — exactly where the two producers used to disagree —
        // plus a removal and a modification, all through the watcher
        makeFile("IMG2.jpg");
        makeFile("img 10.jpg");
        makeFile("10.jpg");
        QVERIFY(QFile::remove(fixtureDir + "/img1.jpg"));
        {
            QFile f(fixtureDir + "/img2.jpg");
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Append));
            QVERIFY(f.write("appended") == 8);
        }
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 5UL, 10000); // 3 - 1 + 3 new
        // A count of 5 is reachable mid-flight (three adds and one removal), so
        // this line only narrows the wait; isSettled() then rules out the things
        // it can actually see — mirrored directory, applied sort, no resync — and
        // the pull below is what waits for the event queue to be empty, because
        // nothing before it can.
        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);
        assertMirrorMatchesEngineExport(dm);

        // under SIZE sort a metadata change re-seats the entry on the engine
        // side; the mirror must follow the wire index, not its own comparator
        dm.setSortingMode(SORT_SIZE);
        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000); // the resort itself is applied
        assertMirrorMatchesEngineExport(dm);

        {
            QFile f(fixtureDir + "/10.jpg");
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Append));
            QVERIFY(f.write(QByteArray(4096, 'x')));
        }
        QTRY_VERIFY_WITH_TIMEOUT(dm.indexOfFile(fixtureDir + "/10.jpg")
                                     > dm.indexOfFile(fixtureDir + "/img2.jpg"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);
        assertMirrorMatchesEngineExport(dm);
    }

    void mutationsOutsideTheMirroredDirectoryAreDropped() {
        // The mirror's vectors are one directory's listing, so a mutation whose
        // path belongs to another folder cannot be an edit of them: the engine's
        // order index means nothing here, and the row would be a file the user
        // never has on screen. The wire carries no directory for a mutation, so
        // the mirror has to make that check itself.
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        auto &bridge = EngineBridge::instance();
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);
        const unsigned long before = dm.fileCount();
        QVERIFY(before > 0);

        QSignalSpy added(&dm, &DirectoryManager::fileAdded);
        emit bridge.dirMutated(QIMGV_MUT_ADDED, QStringLiteral("/some/other/place/x.jpg"),
                               false, 1, 0, 0, QString());
        QCOMPARE(added.count(), 0);
        QCOMPARE(dm.fileCount(), before);
        QVERIFY(!dm.containsFile(QStringLiteral("/some/other/place/x.jpg")));

        // and the shape the engine bug actually produced: the *same name* from
        // another folder. The mirror keys on the full path, so it would not
        // collide — it would simply gain a row for a file that is not here, at
        // an index computed against the other folder's order.
        const QString twin = QFileInfo(dm.filePathAt(0)).fileName();
        emit bridge.dirMutated(QIMGV_MUT_ADDED, QStringLiteral("/some/other/place/") + twin,
                               false, 1, 0, 1, QString());
        QCOMPARE(added.count(), 0);
        QCOMPARE(dm.fileCount(), before);

        // the guard must not swallow the real case
        makeFile("legit.jpg");
        QTRY_VERIFY_WITH_TIMEOUT(dm.containsFile(fixtureDir + "/legit.jpg"), 10000);
    }

    void renameFromOutsideTheMirroredDirectoryIsDropped() {
        // A rename has two subjects, and the destination guard above can only
        // see one of them: the source names the ROW to move, so a foreign
        // source would erase a local entry that has nothing to do with it and
        // then splice a row from another folder at an index computed against
        // that other folder's order.
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        auto &bridge = EngineBridge::instance();
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_VERIFY_WITH_TIMEOUT(dm.isSettled(), 10000);
        const unsigned long before = dm.fileCount();
        QVERIFY(before > 0);
        QSignalSpy renamed(&dm, &DirectoryManager::fileRenamed);
        QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);

        // legal destination, foreign source — the shape the guard exists for
        emit bridge.dirMutated(QIMGV_MUT_RENAMED, fixtureDir + "/img1.jpg", false, 1, 0, 0,
                               QStringLiteral("/some/other/place/x.jpg"));
        QCOMPARE(renamed.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(dm.fileCount(), before);
        QVERIFY(dm.containsFile(fixtureDir + "/img1.jpg"));
        // the row it would have moved is still the engine's own row
        assertMirrorMatchesEngineExport(dm);

        // and a real rename of the same destination still lands
        QVERIFY(QFile::rename(fixtureDir + "/img2.jpg", fixtureDir + "/moved.jpg"));
        QTRY_COMPARE_WITH_TIMEOUT(renamed.count(), 1, 10000);
    }
};

#include "listing_tests.moc"

int runListingTests(int argc, char **argv) {
    TestListing test;
    return runIfOwned(test, argc, argv);
}
