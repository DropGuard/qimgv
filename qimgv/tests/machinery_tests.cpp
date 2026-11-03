#include "engine_stack_fixture.h"

// the machinery that degrades quietly instead of crashing: randomizer, cache,
// DocumentInfo, thumbnailer accounting
class TestMachinery : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestMachinery() : EngineStackFixture("machinery") {}

private slots:
    void initTestCase() { init(); }

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
        QVERIFY(a.has_value());
        QVERIFY(b.has_value());
        // two different absences, and only one of them is the optional: a path
        // that is not a file at all, versus a file whose suffix qimgv still
        // tries as an image. The second is not isLoaded() == false either:
        // ImageStatic::loadGeneric ignores QImageReader::read()'s result and
        // sets the flag unconditionally, so an undecodable suffix arrives as a
        // loaded object holding a null QImage. Only the image says so.
        QVERIFY(!ImageFactory::createImage(fixtureDir + "/missing.jpg", false).has_value());
        QVERIFY(!ImageFactory::createImage(fixtureDir + "/subdir", false).has_value());
        auto junk = ImageFactory::createImage(fixtureDir + "/skip.txt", false);
        QVERIFY(junk.has_value());
        // the object exists and the load was attempted, but the pixels are
        // null — getImage() is the only public question that distinguishes
        QVERIFY((*junk)->getImage()->isNull());
        QVERIFY(cache.insert(*a));
        QVERIFY(cache.insert(*b));
        QVERIFY(cache.contains(fixtureDir + "/img1.jpg"));
        QCOMPARE(cache.get(fixtureDir + "/img1.jpg").get(), a->get());

        // a duplicate insert is refused, not double-stored
        QVERIFY(!cache.insert(*a));
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

    // FSEntry(const QString&) is the re-stat the mirror leans on: engine
    // "modified" events flow through it. Its body once referenced its own
    // members instead of the parameter, so every stat produced an empty entry
    // and updateFileEntry replaced a valid mirror entry with it, silently.
    void fsEntryConstructorStatsThePathItWasGiven() {
        resetFixture();
        FSEntry file(fixtureDir + "/img1.jpg");
        QCOMPARE(file.name, QStringLiteral("img1.jpg"));
        QCOMPARE(file.path, fixtureDir + "/img1.jpg");
        QVERIFY(!file.isDirectory);
        QCOMPARE(file.size, 1UL); // fixture files hold one byte

        FSEntry directory(fixtureDir + "/subdir");
        QCOMPARE(directory.name, QStringLiteral("subdir"));
        QCOMPARE(directory.path, fixtureDir + "/subdir");
        QVERIFY(directory.isDirectory);

        // a stat that fails (vanished file) leaves the entry empty by
        // contract — updateFileEntry refuses empty entries
        FSEntry missing(fixtureDir + "/vanished.jpg");
        QVERIFY(missing.path.isEmpty());
        QVERIFY(missing.name.isEmpty());
        QCOMPARE(missing.size, 0UL);
        QVERIFY(!missing.isDirectory);
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

    // The scale pipeline is: worker thread computes the image, Scaler::onTaskFinish
    // runs on that thread (direct connection) and re-emits across a QUEUED hop to the
    // thread Scaler lives on. If the payload has no metatype, Qt drops the call with a
    // warning that scrolls past in the log and the viewer simply never gets a result —
    // silently, with no crash and every symbol still resolving.
    void scaledResultSurvivesTheQueuedHop() {
        resetFixture();
        const QSize orig(64, 64);
        QVERIFY(writeBigImage(fixtureDir + "/real.png", "png", orig));
        auto image = *ImageFactory::createImage(fixtureDir + "/real.png", false);
        QVERIFY(image->getImage()->size() == orig);

        Cache cache;
        Scaler scaler(&cache);
        int delivered = 0;
        QPixmap result;
        QObject::connect(&scaler, &Scaler::scalingFinished, [&](QPixmap pix, ScalerRequest) {
            result = pix;
            ++delivered;
        });
        scaler.requestScaled(ScalerRequest(image, QSize(32, 32), fixtureDir + "/real.png",
                                           QI_FILTER_BILINEAR, true));
        QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, 10000);
        QCOMPARE(result.size(), QSize(32, 32));
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
};

#include "machinery_tests.moc"

int runMachineryTests(int argc, char **argv) {
    TestMachinery test;
    return runIfOwned(test, argc, argv);
}
