#include "engine_stack_fixture.h"

// the listing arena: model.rs::write_listing against EngineBridge::parseListing,
// written independently of each other, and the SortingMode -> QIMGV_SORT_* map
class TestWireFormat : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestWireFormat() : EngineStackFixture("wire-format") {}

private slots:
    void initTestCase() { init(); }

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
};

#include "wire_format_tests.moc"

int runWireFormatTests(int argc, char **argv) {
    TestWireFormat test;
    return runIfOwned(test, argc, argv);
}
