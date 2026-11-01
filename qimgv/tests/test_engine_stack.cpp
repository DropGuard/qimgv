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
#include <QStandardPaths>

#include "components/directorymanager/directorymanager.h"
#include "components/directorymanager/enginebridge.h"

class TestEngineStack : public QObject {
    Q_OBJECT
    QTemporaryDir homeDir;
    QString fixtureDir;

    void makeFile(const QString &name) {
        QFile f(fixtureDir + "/" + name);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }

private slots:
    void initTestCase() {
        QVERIFY(homeDir.isValid());
        // isolate Settings/QStandardPaths from the real user config
        qputenv("HOME", homeDir.path().toUtf8());
        QVERIFY(Settings::getInstance() != nullptr);

        fixtureDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                     + "/qimgv-engine-stack-test";
        QDir(fixtureDir).removeRecursively();
        QVERIFY(QDir().mkpath(fixtureDir));
        // natural-order-relevant names
        makeFile("img1.jpg");
        makeFile("img2.jpg");
        makeFile("img10.jpg");
        makeFile("skip.txt");
        QVERIFY(QDir(fixtureDir).mkdir("subdir"));
    }

    void cleanupTestCase() {
        QDir(fixtureDir).removeRecursively();
    }

    // --- layer 1: the event pump itself ---------------------------------------

    void engineBridgePumpDeliversListed() {
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
        DirectoryManager dm;
        QVERIFY(!dm.setDirectory("/nonexistent/qimgv-test-42"));
    }

    void sortingResortsAsynchronously() {
        DirectoryManager dm;
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        QSignalSpy resort(&dm, &DirectoryManager::sortingChanged);
        dm.setSortingMode(SORT_NAME_DESC);
        QTRY_COMPARE_WITH_TIMEOUT(resort.count(), 1, 10000);
        QCOMPARE(dm.filePathAt(0), fixtureDir + "/img10.jpg");
        QCOMPARE(dm.filePathAt(2), fixtureDir + "/img1.jpg");
    }

    void liveFileCreationConvergesViaRescan() {
        DirectoryManager dm;
        QSignalSpy added(&dm, &DirectoryManager::fileAdded);
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        // a file appears while the directory is open (watcher -> debounced
        // engine rescan -> diffing resync)
        makeFile("latecomer.jpg");
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 4UL, 10000);
        QCOMPARE(added.count(), 1);
        QCOMPARE(added.at(0).at(0).toString(), fixtureDir + "/latecomer.jpg");
        // the engine listing is authoritative: natural order kept
        QCOMPARE(dm.filePathAt(0), fixtureDir + "/img1.jpg");
        QCOMPARE(dm.filePathAt(3), fixtureDir + "/latecomer.jpg");
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestEngineStack t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_engine_stack.moc"
