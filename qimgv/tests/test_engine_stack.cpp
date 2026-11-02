// Engine stack integration test: EngineBridge event pump + DirectoryManager
// async wiring. This is the layer that has no Rust-side coverage (it is Qt
// event-loop behavior) and no other test — it exists because bridge debugging
// by printf/strace was the slowest iteration loop in the project.
//
// Ownership rule: business semantics live in Rust tests; this file owns the
// Qt-side integration — async delivery through the event loop, signal
// ordering, rescan convergence.

#include <QtTest>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>

#include "components/directorymanager/directorymanager.h"
#include "components/directorymanager/enginebridge.h"
#include "components/directorymodel.h"
#include "components/cache/cache.h"
#include "components/thumbnailer/thumbnailer.h"
#include "sourcecontainers/documentinfo.h"
#include "sourcecontainers/image.h"
#include "utils/folderend.h"
#include "utils/filelog.h"
#include "utils/imagefactory.h"
#include "utils/randomizer.h"
#include "testhelpers.h"
#include "qimgv_engine.h"

class TestEngineStack : public QObject {
    Q_OBJECT
    IsolatedHome home;
    QString fixtureBase;
    QString fixtureDir;
    int slotSeq = 0;

    void makeFile(const QString &name) {
        QFile f(fixtureDir + "/" + name);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }

    // each slot gets its own directory: the engine holds inotify watches on
    // these paths for the whole process, so a watched dir must never be
    // deleted and recreated behind its back
    void resetFixture() {
        ++slotSeq;
        fixtureDir = fixtureBase + QString("/slot%1").arg(slotSeq);
        QVERIFY(QDir().mkpath(fixtureDir));
        makeFile("img1.jpg");
        makeFile("img2.jpg");
        makeFile("img10.jpg");
        makeFile("skip.txt");
        QVERIFY(QDir(fixtureDir).mkdir("subdir"));
    }

private slots:
    void initTestCase() {
        QVERIFY(home.path() != nullptr);
        // IsolatedHome already redirected HOME and the config identity
        QVERIFY(Settings::getInstance() != nullptr);
        // the two-tier tests catch imageReady through QSignalSpy, whose queued
        // recording needs the shared_ptr argument registered
        qRegisterMetaType<std::shared_ptr<Image>>("std::shared_ptr<Image>");

        fixtureBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                      + "/qimgv-engine-stack-test";
        QDir(fixtureBase).removeRecursively();
    }

    void cleanupTestCase() {
        QDir(fixtureBase).removeRecursively();
    }

    // --- layer 1: the event pump itself ---------------------------------------

    void engineBridgePumpDeliversListed() {
        resetFixture();
        auto &bridge = EngineBridge::instance();
        QSignalSpy listed(&bridge, &EngineBridge::dirListed);
        QVERIFY(listed.isValid());

        QVERIFY(bridge.openDir(fixtureDir, 0 /*QIMGV_SORT_NAME*/, false));
        QTRY_COMPARE_WITH_TIMEOUT(listed.count(), 1, 10000);

        QCOMPARE(bridge.fileCount(), 3); // txt filtered
        QCOMPARE(bridge.dirCount(), 1);
        EngineBridge::EngineEntry e;
        QVERIFY(bridge.entryAt(0, false, e));
        QCOMPARE(e.name, QStringLiteral("img1.jpg")); // natural order: 1 < 2 < 10
        QVERIFY(bridge.entryAt(2, false, e));
        QCOMPARE(e.name, QStringLiteral("img10.jpg"));
    }

    // --- layer 2: DirectoryManager async wiring --------------------------------

    void directoryManagerOpenIsAsynchronous() {
        resetFixture();
        DirectoryManager dm;
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
        DirectoryManager dm;
        QVERIFY(!dm.setDirectory("/nonexistent/qimgv-test-42"));
    }

    void sortingResortsAsynchronously() {
        resetFixture();
        DirectoryManager dm;
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
        DirectoryManager dm;
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
        DirectoryManager dm;
        QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        QVERIFY(QFile::remove(fixtureDir + "/img2.jpg"));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 2UL, 10000);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(dm.filePathAt(0), fixtureDir + "/img1.jpg");
        QCOMPARE(dm.filePathAt(1), fixtureDir + "/img10.jpg");
    }

    void liveRenameSurfacesAsRemoveAndAdd() {
        // the engine does not pair inotify rename cookies (v1): a rename
        // surfaces as Removed(old) + Added(new) — ordered and lossless
        resetFixture();
        DirectoryManager dm;
        QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
        QSignalSpy added(&dm, &DirectoryManager::fileAdded);
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        QVERIFY(QFile::rename(fixtureDir + "/img2.jpg", fixtureDir + "/renamed.jpg"));
        QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(added.count(), 1, 10000);
        QCOMPARE(removed.at(0).at(0).toString(), fixtureDir + "/img2.jpg");
        QCOMPARE(added.at(0).at(0).toString(), fixtureDir + "/renamed.jpg");
        QCOMPARE(dm.fileCount(), 3UL);
    }

    void forceInsertSurvivesResortViaInclude() {
        resetFixture();
        DirectoryManager dm;
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        // mime-valid file the extension filter excludes: forceInsert must
        // reach the engine (Include) so later mirrors keep it
        makeFile("mime-ok.xyz");
        QVERIFY(dm.forceInsertFileEntry(fixtureDir + "/mime-ok.xyz"));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 4UL, 10000);

        dm.setSortingMode(SORT_NAME_DESC);
        QTRY_COMPARE_WITH_TIMEOUT(dm.sortingMode() == SORT_NAME_DESC, true, 10000);
        QTest::qWait(400); // allow any (wrongly) converging mirror to land
        QCOMPARE(dm.fileCount(), 4UL);
        QCOMPARE(dm.containsFile(fixtureDir + "/mime-ok.xyz"), true);
    }

    // --- layer 3: the listing wire format -------------------------------------
    // model.rs::write_listing is the only producer and EngineBridge::parseListing
    // the only consumer, written independently of each other. Drift between them
    // shows up as a short or garbled listing, never as a crash — so it has to be
    // asserted by round-tripping the real bytes.

    void listingArenaRoundTripsThroughTheCppParser() {
        resetFixture();
        // multi-byte UTF-8: the arena stores BYTE lengths, so this is what
        // catches a producer/consumer disagreement about chars vs bytes
        makeFile(QStringLiteral("únicøde-日本.jpg"));
        makeFile(QString(120, QChar('x')) + QStringLiteral(".jpg")); // past any fixed buffer

        auto &bridge = EngineBridge::instance();
        QVERIFY(bridge.openDir(fixtureDir, 0 /*QIMGV_SORT_NAME*/, false));
        QTRY_COMPARE_WITH_TIMEOUT(bridge.fileCount(), 5, 10000);
        QCOMPARE(bridge.dirCount(), 1);

        QByteArray arena;
        quint32 fc = 0, dc = 0;
        QVERIFY(bridge.exportListing(arena, fc, dc));
        QCOMPARE(int(fc), bridge.fileCount());
        QCOMPARE(int(dc), bridge.dirCount());

        std::vector<EngineBridge::EngineEntry> files, dirs;
        QVERIFY(EngineBridge::parseListing(arena, files, dirs));
        QCOMPARE(int(files.size()), int(fc));
        QCOMPARE(int(dirs.size()), int(dc));

        // field-by-field against the engine's own index-based reader: same
        // source of truth, two independent code paths
        for (size_t i = 0; i < files.size(); ++i) {
            EngineBridge::EngineEntry ref;
            QVERIFY(bridge.entryAt(int(i), false, ref));
            QCOMPARE(files[i].name, ref.name);
            QCOMPARE(files[i].path, ref.path);
            QCOMPARE(files[i].size, ref.size);
            QCOMPARE(files[i].mtimeMs, ref.mtimeMs);
            QCOMPARE(files[i].isDir, false);
        }
        EngineBridge::EngineEntry dirRef;
        QVERIFY(bridge.entryAt(0, true, dirRef));
        QCOMPARE(dirs[0].name, dirRef.name);
        QCOMPARE(dirs[0].path, dirRef.path);
        QCOMPARE(dirs[0].isDir, true);
    }

    void unicodeNamesSurviveTheArenaByteForByte() {
        resetFixture();
        const QString wide = QStringLiteral("日本-ünicode-🎨.jpg");
        makeFile(wide);

        auto &bridge = EngineBridge::instance();
        QVERIFY(bridge.openDir(fixtureDir, 0, false));
        QTRY_COMPARE_WITH_TIMEOUT(bridge.fileCount(), 4, 10000);

        QByteArray arena;
        quint32 fc = 0, dc = 0;
        QVERIFY(bridge.exportListing(arena, fc, dc));
        std::vector<EngineBridge::EngineEntry> files, dirs;
        QVERIFY(EngineBridge::parseListing(arena, files, dirs));

        bool found = false;
        for (const auto &f : files) {
            if (f.name == wide) {
                found = true;
                QCOMPARE(f.path, fixtureDir + QStringLiteral("/") + wide);
            }
        }
        QVERIFY(found);
    }

    void truncatedArenaStopsAtARecordBoundary() {
        resetFixture();
        auto &bridge = EngineBridge::instance();
        QVERIFY(bridge.openDir(fixtureDir, 0, false));
        QTRY_COMPARE_WITH_TIMEOUT(bridge.fileCount(), 3, 10000);

        QByteArray arena;
        quint32 fc = 0, dc = 0;
        QVERIFY(bridge.exportListing(arena, fc, dc));
        std::vector<EngineBridge::EngineEntry> fullFiles, fullDirs;
        QVERIFY(EngineBridge::parseListing(arena, fullFiles, fullDirs));
        QCOMPARE(int(fullFiles.size()), 3);
        QCOMPARE(int(fullDirs.size()), 1);

        // cut into the final record: the parse must drop it whole and report the
        // mismatch instead of handing back a half-read name or path
        std::vector<EngineBridge::EngineEntry> files, dirs;
        QVERIFY(!EngineBridge::parseListing(arena.left(arena.size() - 3), files, dirs));
        QCOMPARE(int(files.size()), 3);
        QCOMPARE(dirs.size(), size_t(0));
        for (size_t i = 0; i < files.size(); ++i)
            QCOMPARE(files[i].name, fullFiles[i].name);
    }

    void headerCountsDisagreeingWithTheArenaAreRejected() {
        // the failure mode when producer and consumer drift: a header promising
        // entries the bytes do not contain. Must not allocate for them, must not
        // report success with a silently empty listing
        QByteArray arena;
        arena.resize(8);
        auto *h = reinterpret_cast<unsigned char *>(arena.data());
        for (int i = 0; i < 4; ++i)
            h[i] = 0xff; // file_count = 0xffffffff
        // dir_count stays 0

        std::vector<EngineBridge::EngineEntry> files, dirs;
        QVERIFY(!EngineBridge::parseListing(arena, files, dirs));
        QVERIFY(files.empty());
        QVERIFY(dirs.empty());

        // an arena shorter than the header is not a listing at all
        QVERIFY(!EngineBridge::parseListing(QByteArray(), files, dirs));
        QVERIFY(!EngineBridge::parseListing(arena.left(7), files, dirs));

        // header with zero counts IS a complete empty listing
        const QByteArray empty(8, '\0');
        QVERIFY(EngineBridge::parseListing(empty, files, dirs));
        QVERIFY(files.empty());
    }

    // --- layer 4: the SortingMode -> QIMGV_SORT_* mapping ----------------------
    // settings.h orders the modes name, name-desc, size, size-desc, time,
    // time-desc; the wire codes swap the size and time pairs. Mapping them
    // positionally sorts by the wrong column, which never crashes and always
    // looks plausible. The behavior behind each code is pinned from the Rust
    // side (ffi_contract.rs::sort_codes_map_to_the_documented_orders).

    void sortModeMappingMatchesTheWireCodes() {
        // the codes are a documented wire format: pin the values themselves, the
        // same way test_engine_contract.cpp pins struct layouts
        QCOMPARE(QIMGV_SORT_NAME, 0);
        QCOMPARE(QIMGV_SORT_NAME_DESC, 1);
        QCOMPARE(QIMGV_SORT_TIME, 2);
        QCOMPARE(QIMGV_SORT_TIME_DESC, 3);
        QCOMPARE(QIMGV_SORT_SIZE, 4);
        QCOMPARE(QIMGV_SORT_SIZE_DESC, 5);

        QCOMPARE(DirectoryManager::engineSortMode(SORT_NAME), QIMGV_SORT_NAME);
        QCOMPARE(DirectoryManager::engineSortMode(SORT_NAME_DESC), QIMGV_SORT_NAME_DESC);
        QCOMPARE(DirectoryManager::engineSortMode(SORT_TIME), QIMGV_SORT_TIME);
        QCOMPARE(DirectoryManager::engineSortMode(SORT_TIME_DESC), QIMGV_SORT_TIME_DESC);
        QCOMPARE(DirectoryManager::engineSortMode(SORT_SIZE), QIMGV_SORT_SIZE);
        QCOMPARE(DirectoryManager::engineSortMode(SORT_SIZE_DESC), QIMGV_SORT_SIZE_DESC);
    }

    // --- layer 5: the configured folder-boundary behavior -----------------------
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

    // --- layer 6: the folder view and folder-boundary navigation agree -----------
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
            QTRY_VERIFY_WITH_TIMEOUT(dm.sortingMode() == mode, 10000);
            QTest::qWait(400); // let the resort's listing rebuild land

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
        QTRY_VERIFY_WITH_TIMEOUT(dm.sortingMode() == SORT_TIME, 10000);
        QTest::qWait(400); // let the resort's rebuild land

        // a name that belongs in the middle, never at the end
        QVERIFY(QDir(fixtureDir).mkdir("mike"));
        QTRY_COMPARE_WITH_TIMEOUT(dm.dirCount(), 4UL, 10000);
        QTest::qWait(400);

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

    // --- layer 7: file logging machinery (utils/filelog) -----------------------
    // install() is thin glue verified by every session's first log lines; the
    // pieces it composes are drivable here with tiny limits.

    void fileLogFormatCarriesLevelCharAndTimestamp() {
        QRegularExpression shape("^\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d+ I hello\\n$");
        QString info = FileLog::formatLine(QtInfoMsg, "hello");
        if(!shape.match(info).hasMatch())
            qWarning() << "formatLine produced:" << info;
        QVERIFY(shape.match(info).hasMatch());
        QCOMPARE(FileLog::formatLine(QtDebugMsg, "x").section(' ', 2, 2), QStringLiteral("D"));
        QCOMPARE(FileLog::formatLine(QtWarningMsg, "x").section(' ', 2, 2), QStringLiteral("W"));
        QCOMPARE(FileLog::formatLine(QtCriticalMsg, "x").section(' ', 2, 2), QStringLiteral("C"));
        QCOMPARE(FileLog::formatLine(QtFatalMsg, "x").section(' ', 2, 2), QStringLiteral("F"));
    }

    void fileLogRotatesPartsAndStopsAtTheCap() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // 31 bytes per line (30 'x' + '\n'); the banner is never counted, so
        // the rotation boundary is exactly where the handler's bytes land
        FileLog::SessionLog log(100, 3);
        QVERIFY(log.open(dir.path() + "/qimgv-test.log"));
        const QString line(30, 'x');
        for(int i = 0; i < 3; ++i)
            log.write(line);
        QCOMPARE(log.part(), 1);
        log.write(line);                       // 124 >= 100 -> part 2
        QCOMPARE(log.part(), 2);
        QCOMPARE(log.bytesInPart(), qint64(0));
        for(int i = 0; i < 4; ++i)
            log.write(line);                   // the 4th write rotates -> part 3
        QCOMPARE(log.part(), 3);
        for(int i = 0; i < 4; ++i)
            log.write(line);                   // at the cap: this write lands, then stop
        QVERIFY(!log.isWriting());
        QCOMPARE(QDir(dir.path())
                     .entryList(QStringList() << "qimgv-test.log*", QDir::Files)
                     .size(), 3);
    }

    void fileLogPruneKeepsTheNewestTenSessions() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for(int i = 1; i <= 12; ++i) {
            QFile f(dir.path() + QString("/qimgv-0101-%1.log").arg(i, 2, 10, QChar('0')));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        FileLog::pruneOldSessions(dir.path(), 10);
        QStringList left = QDir(dir.path())
                               .entryList(QStringList() << "qimgv-*.log*", QDir::Files, QDir::Name);
        QCOMPARE(left.size(), 10);
        QCOMPARE(left.first(), QStringLiteral("qimgv-0101-03.log"));
        QCOMPARE(left.last(), QStringLiteral("qimgv-0101-12.log"));
    }

    // --- layer 8: the two-tier decode pipeline (DirectoryModel + Loader) -------
    // Slice 4's promises where they live: tier-1 is a screen-sized decode and
    // JPEG-only (Qt scales every other format AFTER a full decode, which costs
    // more than it saves — measured, and the reason for the gate); tier-2 is
    // the full-res upgrade of a tier-1 cached image. The queued upgrade that
    // the next flip's clearPool kills is deliberately not asserted: an idle
    // pool thread can start the decode before the flip lands, so that behavior
    // is a race by construction and belongs to the real-machine logs.

    QSize offscreenScreenPixels() {
        QScreen *screen = QApplication::primaryScreen();
        Q_ASSERT(screen != nullptr); // the offscreen platform always provides one
        return screen->size() * screen->devicePixelRatio();
    }

    bool writeBigImage(const QString &path, const QString &format, const QSize &size) {
        QImage img(size, QImage::Format_RGB32);
        QPainter p(&img);
        QLinearGradient g(0, 0, size.width(), size.height());
        g.setColorAt(0.0, Qt::red);
        g.setColorAt(1.0, Qt::blue);
        p.fillRect(img.rect(), g);
        p.end();
        return img.save(path, format.toStdString().c_str());
    }

    std::shared_ptr<Image> spyImage(const QSignalSpy &spy, int row) {
        return spy.at(row).at(0).value<std::shared_ptr<Image>>();
    }

    void jpegTwoTierDecodesScaledThenUpgradesToFull() {
        resetFixture();
        const QSize screen = offscreenScreenPixels();
        const QSize orig = screen * 2;
        QVERIFY(writeBigImage(fixtureDir + "/big.jpg", "jpg", orig));

        DirectoryModel model;
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(ready.isValid());
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.jpg", true); // tier-1 by default
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
        QCOMPARE(ready.at(0).at(2).toBool(), true);
        QCOMPARE(spyImage(ready, 0)->size(), screen);

        model.upgradeToFull(fixtureDir + "/big.jpg");
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 2, 10000);
        QCOMPARE(ready.at(1).at(2).toBool(), false);
        QCOMPARE(spyImage(ready, 1)->size(), orig);
    }

    void pngDecodesFullQualityInOneTier() {
        // regression: the scaled request once applied to every format. Qt
        // scales PNG after a full decode, so the "fast" tier was slower than
        // the thing it replaced (84ms vs 46ms measured). The gate is
        // JPEG-only, and this pins it.
        resetFixture();
        const QSize orig = offscreenScreenPixels() * 2;
        QVERIFY(writeBigImage(fixtureDir + "/big.png", "png", orig));

        DirectoryModel model;
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.png", true);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
        QTest::qWait(600); // a second tier would have arrived by now
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.at(0).at(2).toBool(), false);
        QCOMPARE(spyImage(ready, 0)->size(), orig);
    }

    void syncLoadIsAlwaysFullQuality() {
        // the editing path: asyncHint=false bypasses the tiers entirely
        resetFixture();
        const QSize orig = offscreenScreenPixels() * 2;
        QVERIFY(writeBigImage(fixtureDir + "/big.jpg", "jpg", orig));

        DirectoryModel model;
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.jpg", false);
        // synchronous: the signal has fired by the time load() returned
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.at(0).at(2).toBool(), false);
        QCOMPARE(spyImage(ready, 0)->size(), orig);
    }

    void cachedFullImageReflipsWithoutRedecoding() {
        resetFixture();
        const QSize screen = offscreenScreenPixels();
        QVERIFY(writeBigImage(fixtureDir + "/big.jpg", "jpg", screen * 2));

        DirectoryModel model;
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.jpg", true);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
        model.upgradeToFull(fixtureDir + "/big.jpg");
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 2, 10000);
        QVERIFY(!ready.at(1).at(2).toBool());

        // flipping back to a fully-cached image serves it from the cache and
        // schedules nothing: mScaledCache forgot the path at the upgrade
        ready.clear();
        model.load(fixtureDir + "/big.jpg", true);
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.at(0).at(2).toBool(), false);
        QTest::qWait(300);
        QCOMPARE(ready.count(), 1);
    }

    void upgradeToFullRefusesWhatWasNeverScaled() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/big.png", "png", offscreenScreenPixels() * 2));

        DirectoryModel model;
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.png", true);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
        ready.clear();

        // a full-res image has left mScaledCache: the upgrade is a no-op,
        // not a third decode of the same file
        model.upgradeToFull(fixtureDir + "/big.png");
        QTest::qWait(400);
        QCOMPARE(ready.count(), 0);

        // and a path outside the listing goes nowhere
        model.upgradeToFull(fixtureDir + "/missing.jpg");
        QTest::qWait(200);
        QCOMPARE(ready.count(), 0);
    }

    // --- layer 9: the machinery where breakage does not crash ------------------
    // Randomizer, Cache and the settings getters fail silently by design: a
    // stuck loop hangs the UI thread, an out-of-bounds index reads garbage, a
    // junk config value flows into a switch that has no matching arm. Every
    // test here pins a failure mode of that shape.

    void randomizerWalksEveryImageOncePerCycle() {
        // the reshuffle reorders the whole vector, so a repeat across a
        // reshuffle boundary is by design; what must never happen is the same
        // image twice IN A ROW (permutations give consecutive positions
        // distinct values, and the reshuffle repositions the current item
        // without moving the draw onto its own value) — plus staying in range
        Randomizer r;
        r.setCount(5);
        r.shuffle();
        int previous = -1;
        for(int i = 0; i < 50; ++i) {
            int v = r.next();
            QVERIFY2(v >= 0 && v < 5, "next() left the index range");
            QVERIFY2(v != previous, "the same image drawn twice in a row");
            previous = v;
        }
    }

    void randomizerSurvivesASingleImageFolder() {
        // vec.size()==1 makes next()/prev()'s "reshuffle when at the edge"
        // condition permanently true: the loop spun forever, hanging the UI
        // thread of anyone pressing next in a one-photo folder with shuffle on
        Randomizer r;
        r.setCount(1);
        QCOMPARE(r.next(), 0);
        QCOMPARE(r.next(), 0);
        QCOMPARE(r.prev(), 0);
    }

    void randomizerSurvivesEmptyAndShrinkingCounts() {
        Randomizer empty;
        empty.setCount(0);
        QCOMPARE(empty.next(), 0); // was an out-of-bounds vec[0] read
        QCOMPARE(empty.prev(), 0);

        Randomizer shrink;
        shrink.setCount(5);
        for(int i = 0; i < 4; ++i)
            shrink.next();          // cursor deep inside the vector
        shrink.setCount(3);         // folder shrank underneath it
        for(int i = 0; i < 6; ++i) {
            int v = shrink.next();
            QVERIFY2(v >= 0 && v < 3, "stale cursor survived setCount shrink");
        }
    }

    void randomizerPrevDoesNotIndexBelowZero() {
        // syncRandomizer() calls setCurrent(indexOfFile(current)), and indexOf
        // returns -1 when the file is not in the listing; prev() then walked
        // to vec[-2]
        Randomizer r;
        r.setCount(4);
        r.setCurrent(999);
        int v = r.prev();
        QVERIFY(v >= 0 && v < 4);
    }

    void cacheRoundTripsInsertsAndTrims() {
        resetFixture();
        Cache cache;
        auto a = ImageFactory::createImage(fixtureDir + "/img1.jpg", false);
        auto b = ImageFactory::createImage(fixtureDir + "/img2.jpg", false);
        QVERIFY(a != nullptr);
        QVERIFY(cache.insert(a));
        QVERIFY(cache.insert(b));
        QVERIFY(cache.contains(fixtureDir + "/img1.jpg"));
        QCOMPARE(cache.get(fixtureDir + "/img1.jpg").get(), a.get());

        // a duplicate insert is refused, not double-stored
        QVERIFY(!cache.insert(a));
        QCOMPARE(cache.keys().size(), 2);

        // a null image must be refused: reporting success for a failed decode
        // is exactly the silent-failure shape this suite hunts
        QVERIFY(!cache.insert(nullptr));
        QCOMPARE(cache.keys().size(), 2);

        cache.trimTo(QStringList() << fixtureDir + "/img2.jpg");
        QVERIFY(!cache.contains(fixtureDir + "/img1.jpg"));
        QVERIFY(cache.contains(fixtureDir + "/img2.jpg"));

        cache.remove(fixtureDir + "/img2.jpg");
        QVERIFY(cache.keys().isEmpty());
    }

    void documentInfoClassifiesFormatsAndAnimation() {
        resetFixture();
        auto write = [this](const QString &name, const QByteArray &bytes) {
            QFile f(fixtureDir + "/" + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
        };

        // animated WebP: VP8X flags bit 1
        QByteArray webpAnimBytes;
        webpAnimBytes.append("RIFF");
        quint32 riffSize = 4 + 4 + 4 + 4 + 1; // WEBP + VP8X + skip + flags
        webpAnimBytes.append(reinterpret_cast<const char *>(&riffSize), 4);
        webpAnimBytes.append("WEBP");
        webpAnimBytes.append("VP8X");
        webpAnimBytes.append(QByteArray(4, '\x00'));
        webpAnimBytes.append('\x02');
        write("anim.webp", webpAnimBytes);
        DocumentInfo animatedWebp(fixtureDir + "/anim.webp");
        QCOMPARE(animatedWebp.type(), DocumentType::ANIMATED);
        QCOMPARE(animatedWebp.format(), QStringLiteral("webp"));

        QByteArray webpStaticBytes = webpAnimBytes;
        webpStaticBytes[webpStaticBytes.size() - 1] = '\x00';
        write("still.webp", webpStaticBytes);
        DocumentInfo staticWebp(fixtureDir + "/still.webp");
        QCOMPARE(staticWebp.type(), DocumentType::STATIC);

        // APNG: acTL inside the first 120 bytes. Whether the "apng" format is
        // produced depends on the deployed Qt imageformats plugin, so the
        // expectation mirrors the plugin check the production code performs.
        QByteArray png = QByteArray("\x89PNG\r\n\x1a\n");
        png.append(QByteArray(20, '\x00'));
        png.append("acTL");
        write("maybe-apng.png", png);
        DocumentInfo apng(fixtureDir + "/maybe-apng.png");
        if(QImageReader::supportedImageFormats().contains("apng")) {
            QCOMPARE(apng.type(), DocumentType::ANIMATED);
            QCOMPARE(apng.format(), QStringLiteral("apng"));
        } else {
            QCOMPARE(apng.type(), DocumentType::STATIC);
            QCOMPARE(apng.format(), QStringLiteral("png"));
        }

        // AVIF animation lives in the ftyp brand; the mime database must know
        // it first, and old shared-mime-info versions do not
        QByteArray avif;
        avif.append("\x00\x00\x00\x20", 4);
        avif.append("ftypavis");
        avif.append(QByteArray(24, '\x00'));
        write("anim.avif", avif);
        QMimeDatabase mimeDb;
        if(mimeDb.mimeTypeForFile(fixtureDir + "/anim.avif",
                                  QMimeDatabase::MatchContent).name()
           == QStringLiteral("image/avif")) {
            DocumentInfo avif(fixtureDir + "/anim.avif");
            QCOMPARE(avif.type(), DocumentType::ANIMATED);
        } else {
            QSKIP("this mime database does not identify ftypavis as image/avif");
        }

        // a .jfif file falls through content detection to the suffix, which
        // the code maps onto jpg
        write("photo.jfif", "not really a jpeg");
        DocumentInfo jfif(fixtureDir + "/photo.jfif");
        QCOMPARE(jfif.format(), QStringLiteral("jpg"));
        QCOMPARE(jfif.type(), DocumentType::STATIC);

        write("frame.gif", "GIF89a-not-really");
        DocumentInfo gif(fixtureDir + "/frame.gif");
        QCOMPARE(gif.type(), DocumentType::ANIMATED);
        QCOMPARE(gif.format(), QStringLiteral("gif"));

        write("pic.bmp", "BM-junk");
        DocumentInfo bmp(fixtureDir + "/pic.bmp");
        QCOMPARE(bmp.type(), DocumentType::STATIC);
        QCOMPARE(bmp.format(), QStringLiteral("bmp"));
    }

    void settingsEnumGettersClampJunkConfigValues() {
        // a hand-edited config file can hold anything; every enum getter must
        // land on a valid member instead of handing garbage to a switch
        settings->setSortingMode(static_cast<SortingMode>(42));
        QVERIFY(settings->sortingMode() == SORT_NAME);
        settings->setSortingMode(static_cast<SortingMode>(-1));
        QVERIFY(settings->sortingMode() == SORT_NAME);
        settings->setSortingMode(SORT_NAME);

        settings->setPanelPosition(static_cast<PanelPosition>(42));
        QVERIFY(settings->panelPosition() == PANEL_RIGHT); // documented fallback
        settings->setPanelPosition(PANEL_TOP);

        // folderEndAction clamping is pinned in its own test; this is the
        // same trap one layer down
        settings->setFolderEndAction(static_cast<FolderEndAction>(99));
        QVERIFY(settings->folderEndAction() == FOLDER_END_NO_ACTION);
        settings->setFolderEndAction(FOLDER_END_NO_ACTION);
    }

    void thumbnailerAccountingSurvivesAClearMidFlight() {
        resetFixture();
        Thumbnailer thumb;
        QSignalSpy ready(&thumb, &Thumbnailer::thumbnailReady);
        QVERIFY(ready.isValid());

        // every request must come back through the engine's scheduler
        thumb.getThumbnailAsync(fixtureDir + "/img1.jpg", 32, false, false);
        thumb.getThumbnailAsync(fixtureDir + "/img2.jpg", 32, false, false);
        // a duplicate collapses instead of double-dispatching
        thumb.getThumbnailAsync(fixtureDir + "/img1.jpg", 32, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 10000);
        QTest::qWait(200);
        QCOMPARE(ready.count(), 2);

        // the clear happens before the event loop drains anything, so the
        // third request is provably still in flight: clearTasks must report
        // it back to the engine as done, or its slot leaks and the pipeline
        // silently stalls forever after
        thumb.getThumbnailAsync(fixtureDir + "/img10.jpg", 32, false, false);
        thumb.clearTasks();

        // the pipeline still works after the clear: a fresh request dispatches
        // and completes (img10's stale dispatch may also complete on drain,
        // so assert a lower bound, not an exact count)
        thumb.getThumbnailAsync(fixtureDir + "/img1.jpg", 64, false, false);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 3, 10000);
        thumb.waitForDone();
    }

    // --- layer 10: the seams the test infra itself stands on -------------------

    void bridgeInstancesAreIsolated() {
        // the old singleton forced every test through one engine, one watch
        // target and one session epoch — the per-slot fixture dirs exist only
        // because of that. Two plain instances now get independent engines:
        resetFixture();
        QVERIFY(QDir(fixtureDir).mkdir("other"));
        QFile other(fixtureDir + "/other/z.jpg");
        QVERIFY(other.open(QIODevice::WriteOnly));

        EngineBridge bridgeA;
        EngineBridge bridgeB;
        QSignalSpy listedA(&bridgeA, &EngineBridge::dirListed);
        QSignalSpy listedB(&bridgeB, &EngineBridge::dirListed);
        QVERIFY(listedA.isValid());
        QVERIFY(listedB.isValid());

        QVERIFY(bridgeA.openDir(fixtureDir, 0, false));
        QVERIFY(bridgeB.openDir(fixtureDir + "/other", 0, false));

        QTRY_COMPARE_WITH_TIMEOUT(listedA.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(listedB.count(), 1, 10000);
        QCOMPARE(listedA.at(0).at(0).toString(), fixtureDir);
        QCOMPARE(listedB.at(0).at(0).toString(), fixtureDir + "/other");
        // and the listings do not cross: each instance reports its own engine
        QCOMPARE(bridgeA.fileCount(), 3);
        QCOMPARE(bridgeB.fileCount(), 1);
    }

    void extensionListForJoinsFormatsAndEmptiesToAcceptAll() {
        // the seam where an empty whitelist once meant "accept nothing"
        // against the header's documented NULL contract
        QCOMPARE(EngineBridge::extensionListFor({QByteArray("jpg"), QByteArray("png")}),
                 QByteArray("jpg\npng"));
        QCOMPARE(EngineBridge::extensionListFor({}), QByteArray());
    }

    void freshSettingsGuardIsolatesConfigState() {
        // without the guard, every test had to reset the keys it depends on
        // and the suite was silently order-sensitive
        settings->setSortingMode(SORT_TIME);
        QVERIFY(settings->sortingMode() == SORT_TIME);
        {
            FreshSettings fresh; // swaps the global to a defaults-only instance
            QVERIFY(settings->sortingMode() == SORT_NAME);
            settings->setSortingMode(SORT_SIZE_DESC);
            QVERIFY(settings->sortingMode() == SORT_SIZE_DESC);
        }
        // the original instance and its in-memory state are back
        QVERIFY(settings->sortingMode() == SORT_TIME);
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestEngineStack t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_engine_stack.moc"
