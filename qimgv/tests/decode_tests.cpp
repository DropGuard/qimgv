#include "engine_stack_fixture.h"

// the two-tier decode pipeline: DirectoryModel plus Loader
// Slice 4's promises where they live: tier-1 is a screen-sized decode and
// JPEG-only (Qt scales every other format AFTER a full decode, which costs
// more than it saves — measured, and the reason for the gate); tier-2 is
// the full-res upgrade of a tier-1 cached image. The queued upgrade that
// the next flip's clearPool kills is deliberately not asserted: an idle
// pool thread can start the decode before the flip lands, so that behavior
// is a race by construction and belongs to the real-machine logs.
class TestDecode : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestDecode() : EngineStackFixture("decode") {}

private slots:
    void initTestCase() { init(); }

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
};

#include "decode_tests.moc"

int runDecodeTests(int argc, char **argv) {
    TestDecode test;
    return runIfOwned(test, argc, argv);
}
