#include "engine_stack_fixture.h"
#include "components/scriptmanager/scriptmanager.h"
#include "utils/inputmap.h"
#include "shortcutbuilder.h"
#include "components/thumbnailer/thumbnailerrunnable.h"
#include "components/cache/thumbnailcache.h"
#include "components/workerpool/workerpool.h"
#include "components/loader/loader.h"

#include <QCoreApplication>
#include <QSet>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QProcess>

#include <atomic>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

namespace {

// occupies its worker until released, so a test decides what stays queued
class HoldJob : public PoolJob {
public:
    HoldJob(std::atomic_bool *_gate, std::atomic_int *_starts)
        : gate(_gate), starts(_starts) {}
    void run() override {
        starts->fetch_add(1);
        while(!gate->load())
            QThread::msleep(1);
    }
private:
    std::atomic_bool *gate;
    std::atomic_int *starts;
};

} // namespace

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
        // the factory now refuses pixel-less objects, so the cache fixtures
        // must be real decodable images, not one-byte listing placeholders
        QVERIFY(writeBigImage(fixtureDir + "/img1.jpg", "jpg", QSize(8, 8)));
        QVERIFY(writeBigImage(fixtureDir + "/img2.jpg", "jpg", QSize(8, 8)));
        Cache cache;
        auto a = ImageFactory::createImage(fixtureDir + "/img1.jpg", false, loadPolicy());
        auto b = ImageFactory::createImage(fixtureDir + "/img2.jpg", false, loadPolicy());
        QVERIFY(a.has_value());
        QVERIFY(b.has_value());
        // every absence is the optional: a missing file, a directory, and a
        // file whose suffix qimgv still tries but cannot decode — the factory
        // never hands out an object whose pixels do not exist, so no consumer
        // downstream ever needs a null-check
        QVERIFY(!ImageFactory::createImage(fixtureDir + "/missing.jpg", false, loadPolicy()).has_value());
        QVERIFY(!ImageFactory::createImage(fixtureDir + "/subdir", false, loadPolicy()).has_value());
        QVERIFY(!ImageFactory::createImage(fixtureDir + "/skip.txt", false, loadPolicy()).has_value());
        // the animated side holds the same contract
        QByteArray junkAnim = "GIF89a-not-really";
        QFile jf(fixtureDir + "/broken.gif");
        QVERIFY(jf.open(QIODevice::WriteOnly));
        jf.write(junkAnim);
        jf.close();
        QVERIFY(!ImageFactory::createImage(fixtureDir + "/broken.gif", false, loadPolicy()).has_value());
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
        DocumentInfo animatedWebp(fixtureDir + "/anim.webp", LoadPolicy::fromSettings());
        QCOMPARE(animatedWebp.type(), DocumentType::ANIMATED);
        QCOMPARE(animatedWebp.format(), QStringLiteral("webp"));

        QByteArray webpStaticBytes = webpAnimBytes;
        webpStaticBytes[webpStaticBytes.size() - 1] = '\x00';
        write("still.webp", webpStaticBytes);
        DocumentInfo staticWebp(fixtureDir + "/still.webp", LoadPolicy::fromSettings());
        QCOMPARE(staticWebp.type(), DocumentType::STATIC);

        // APNG: acTL inside the first 120 bytes. Whether the "apng" format is
        // produced depends on the deployed Qt imageformats plugin, so the
        // expectation mirrors the plugin check the production code performs.
        QByteArray png = QByteArray("\x89PNG\r\n\x1a\n");
        png.append(QByteArray(20, '\x00'));
        png.append("acTL");
        write("maybe-apng.png", png);
        DocumentInfo apng(fixtureDir + "/maybe-apng.png", LoadPolicy::fromSettings());
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
            DocumentInfo avifDoc(fixtureDir + "/anim.avif", LoadPolicy::fromSettings());
            QCOMPARE(avifDoc.type(), DocumentType::ANIMATED);
        } else {
            QSKIP("this mime database does not identify ftypavis as image/avif");
        }

        // a .jfif file falls through content detection to the suffix, which
        // the code maps onto jpg
        write("photo.jfif", "not really a jpeg");
        DocumentInfo jfif(fixtureDir + "/photo.jfif", LoadPolicy::fromSettings());
        QCOMPARE(jfif.format(), QStringLiteral("jpg"));
        QCOMPARE(jfif.type(), DocumentType::STATIC);

        write("frame.gif", "GIF89a-not-really");
        DocumentInfo gif(fixtureDir + "/frame.gif", LoadPolicy::fromSettings());
        QCOMPARE(gif.type(), DocumentType::ANIMATED);
        QCOMPARE(gif.format(), QStringLiteral("gif"));

        write("pic.bmp", "BM-junk");
        DocumentInfo bmp(fixtureDir + "/pic.bmp", LoadPolicy::fromSettings());
        QCOMPARE(bmp.type(), DocumentType::STATIC);
        QCOMPARE(bmp.format(), QStringLiteral("bmp"));
    }

    // R4's fix pattern, pinned: classification follows the CARRIED policy, not
    // ambient state — the same file classifies differently under two policies,
    // and no pool thread ever had to ask the settings global
    void documentInfoClassificationFollowsTheCarriedPolicy() {
        resetFixture();
        makeFile("clip.mp4");

        LoadPolicy videoOn;
        videoOn.videoPlayback = true;
        videoOn.videoFormats.insert("video/mp4", "mp4");
        DocumentInfo videoed(fixtureDir + "/clip.mp4", videoOn);
        QCOMPARE(videoed.type(), DocumentType::VIDEO);
        QCOMPARE(videoed.format(), QStringLiteral("mp4"));

        LoadPolicy videoOff;
        videoOff.videoPlayback = false;
        DocumentInfo plain(fixtureDir + "/clip.mp4", videoOff);
        QCOMPARE(plain.type(), DocumentType::STATIC);
        QCOMPARE(plain.format(), QStringLiteral("mp4"));
    }

    // ScriptManager's blocking path used to swallow a failed start behind a
    // misspelled qDebug ("Unable not run") while the caller waited on a process
    // that would never run — the detached path announced failures, this one
    // did not.
    void scriptBlockingFailureIsAnnounced() {
        auto sm = ScriptManager::getInstance();
        QSignalSpy errored(sm, &ScriptManager::error);
        QVERIFY(errored.isValid());
        sm->addScript("no-such-tool", Script("qimgv-no-such-binary-42 --flag", true));
        sm->runScript("no-such-tool", nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(errored.count(), 1, 5000);
        QVERIFY(errored.at(0).at(0).toString().contains("qimgv-no-such-binary-42"));
        sm->removeScript("no-such-tool");
    }

    void scriptBlockingRunCompletesWithoutError() {
        auto sm = ScriptManager::getInstance();
        QSignalSpy errored(sm, &ScriptManager::error);
        QVERIFY(errored.isValid());
        sm->addScript("true-bin", Script("/bin/true", true));
        sm->runScript("true-bin", nullptr);
        QTest::qWait(300); // runScript blocks inside; this only catches late errors
        QCOMPARE(errored.count(), 0);
        sm->removeScript("true-bin");
    }

    void splitCommandLineKeepsQuotedArguments() {
        auto parts = ScriptManager::splitCommandLine(QString::fromUtf8(R"(cmd "two words" tail)"));
        QCOMPARE(parts.size(), 3);
        QCOMPARE(parts.at(1), QStringLiteral("two words"));
        QCOMPARE(parts.at(2), QStringLiteral("tail"));
    }

    void fileManagerSelectCommandKeepsPathWithSpacesAsOneArgument() {
        const QString path = QStringLiteral("/mnt/data/a b/name-0024.mp4");
        auto parts = ScriptManager::splitCommandLine(
            ScriptManager::fileManagerSelectCommand(QStringLiteral("dolphin"), path));
        QCOMPARE(parts.size(), 3);
        QCOMPARE(parts.at(1), QStringLiteral("--select"));
        QCOMPARE(parts.at(2), path);
        // the pre-fix call-site form, kept as the counterexample: unquoted, the
        // path splits at its space and the file manager receives a truncated
        // nonexistent argument -- that is what broke "show in folder"
        auto broken = ScriptManager::splitCommandLine(QStringLiteral("dolphin --select ") + path);
        QCOMPARE(broken.size(), 4);
    }

    void thumbnailCachePurgesByIdleTimeAndReadRefreshesTheClock() {
        resetFixture();
        // a purge deletes files and the byte assertions count every file in
        // the directory, so the directory belongs to this test alone — a
        // fixture shared with another suite, or with the same suite's previous
        // process, silently changes the census
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("idlecache"));
        QVERIFY(QDir().mkpath(QFileInfo(cache.thumbnailPath(QStringLiteral("x"))).absolutePath()));
        QImage dot(4, 4, QImage::Format_ARGB32);
        dot.fill(Qt::black);
        const QDateTime now = QDateTime::currentDateTime();
        auto age = [&now](const QString &path, int days) {
            const struct timespec times[2] = { { 0, UTIME_OMIT },
                                               { now.addDays(-days).toSecsSinceEpoch(), 0 } };
            return ::utimensat(AT_FDCWD, path.toLocal8Bit().constData(), times, 0) == 0;
        };

        cache.saveThumbnail(&dot, QStringLiteral("hot"));
        cache.saveThumbnail(&dot, QStringLiteral("stale"));
        QVERIFY(age(cache.thumbnailPath(QStringLiteral("hot")), 8));
        QVERIFY(age(cache.thumbnailPath(QStringLiteral("stale")), 16));
        // a cap nobody can reach: this is the idle pass alone, unchanged
        auto report = cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes);
        QCOMPARE(report.idleDropped, 1);
        QCOMPARE(report.sizeEvicted, 0);
        QVERIFY(cache.exists(QStringLiteral("hot")));
        QVERIFY(!cache.exists(QStringLiteral("stale")));

        // a read hit refreshes mtime: a 20-day-old entry that gets displayed
        // once survives the next sweep -- idle time, not age
        cache.saveThumbnail(&dot, QStringLiteral("revived"));
        QVERIFY(age(cache.thumbnailPath(QStringLiteral("revived")), 20));
        QImage *hit = cache.readThumbnail(QStringLiteral("revived"));
        QVERIFY(hit);
        delete hit;
        QCOMPARE(cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes).idleDropped, 0);
        QVERIFY(cache.exists(QStringLiteral("revived")));
    }

    // The byte cap is a fuse behind the idle sweep, not a second policy: it may
    // only fire on what the sweep left, it must order eviction by the same
    // last-used clock the expiry uses, and the boundary has to be exact — a
    // cache sitting AT the cap is a cache that is not over it.
    void thumbnailCacheByteCapFiresOnlyOnWhatTheIdleSweepLeaves() {
        resetFixture();
        QTemporaryDir ownCap;
        QVERIFY(ownCap.isValid());
        QVERIFY(QDir().mkpath(ownCap.filePath("capcache")));
        ThumbnailCache cache(ownCap.filePath("capcache"));
        QImage dot(4, 4, QImage::Format_ARGB32);
        dot.fill(Qt::green);
        const QDateTime now = QDateTime::currentDateTime();
        auto age = [&now](const QString &path, int days) {
            const struct timespec times[2] = { { 0, UTIME_OMIT },
                                               { now.addDays(-days).toSecsSinceEpoch(), 0 } };
            return ::utimensat(AT_FDCWD, path.toLocal8Bit().constData(), times, 0) == 0;
        };
        auto size = [](const QString &path) { return QFileInfo(path).size(); };
        const QString newest = cache.thumbnailPath(QStringLiteral("newest"));
        const QString older  = cache.thumbnailPath(QStringLiteral("older"));
        const QString older2 = cache.thumbnailPath(QStringLiteral("older2"));
        const QString oldest = cache.thumbnailPath(QStringLiteral("oldest"));

        // four live entries (all inside the idle window), aged so the eviction
        // order is unambiguous: oldest is the one the LRU gives up first
        cache.saveThumbnail(&dot, QStringLiteral("newest"));
        cache.saveThumbnail(&dot, QStringLiteral("older"));
        cache.saveThumbnail(&dot, QStringLiteral("older2"));
        cache.saveThumbnail(&dot, QStringLiteral("oldest"));
        QVERIFY(age(newest, 1));
        QVERIFY(age(older, 2));
        QVERIFY(age(older2, 3));
        QVERIFY(age(oldest, 4));
        const qint64 four = size(newest) + size(older) + size(older2) + size(oldest);
        const qint64 twoNewest = size(newest) + size(older);
        QVERIFY2(four > twoNewest, "fixture degenerate: entries have no size to evict");

        // at the cap: nothing fires, nothing is touched
        auto atCap = cache.purgeOlderThan(15, four);
        QCOMPARE(atCap.idleDropped, 0);
        QCOMPARE(atCap.sizeEvicted, 0);
        QCOMPARE(atCap.bytesAfter, four);

        // under it: evicted oldest-first, and it stops the moment the total is
        // inside the cap — the two newest are the survivors because they are
        // the two most recently used
        auto overCap = cache.purgeOlderThan(15, twoNewest);
        QCOMPARE(overCap.idleDropped, 0);      // none of these was idle-expired
        QCOMPARE(overCap.sizeEvicted, 2);
        QCOMPARE(overCap.bytesAfter, twoNewest);
        QVERIFY(cache.exists(QStringLiteral("newest")));
        QVERIFY(cache.exists(QStringLiteral("older")));
        QVERIFY(!cache.exists(QStringLiteral("older2")));
        QVERIFY(!cache.exists(QStringLiteral("oldest")));

        // the ordering claim, and the reason the sweep runs first: a 30-day-old
        // entry is removed as expired, not as oversized, and its bytes never
        // count toward the cap — so a cache whose overage IS the junk the sweep
        // already removed evicts nothing
        cache.saveThumbnail(&dot, QStringLiteral("ancient"));
        QVERIFY(age(cache.thumbnailPath(QStringLiteral("ancient")), 30));
        const qint64 twoPlusAncient = twoNewest + size(cache.thumbnailPath(QStringLiteral("ancient")));
        auto mixed = cache.purgeOlderThan(15, twoPlusAncient - 1);
        QCOMPARE(mixed.idleDropped, 1);          // the ancient one, by expiry
        QCOMPARE(mixed.sizeEvicted, 0);          // and the fuse never needed to fire
        QVERIFY(cache.exists(QStringLiteral("newest")));
        QVERIFY(cache.exists(QStringLiteral("older")));
        QVERIFY(!cache.exists(QStringLiteral("ancient")));
    }

    void processArgumentsSubstitutesTheFileTag() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/tagged.png", "png", QSize(8, 8)));
        auto img = *ImageFactory::createImage(fixtureDir + "/tagged.png", false, loadPolicy());
        QStringList cmd = { "tool", "%file%" };
        ScriptManager::processArguments(cmd, img);
        QCOMPARE(cmd.at(1), fixtureDir + "/tagged.png");
    }

    // Loader::clearTasks() is the user leaving a folder: the queued half comes
    // back from drain() and must never be seen again — and a job that stops at
    // its entry checkpoint reports nothing, so no loadFinished/loadFailed can
    // outlive the leaving and turn "no longer wanted" into "this file is
    // broken". The pool's single worker is held by a Scaler-owner job, which
    // keeps the Loader job queued deterministically AND lets clearTasks()'s
    // own waitForOwner(Loader) return immediately, since nothing of Loader's
    // is running.
    void loaderClearTasksDropsQueuedJobsWithoutAReport() {
        resetFixture();
        std::atomic_bool gate{false};
        std::atomic_int holds{0}, finished{0}, failed{0};
        WorkerPool pool(1);
        pool.submit(PoolOwner::Scaler, 0, std::make_unique<HoldJob>(&gate, &holds));
        QTRY_COMPARE_WITH_TIMEOUT(holds.load(), 1, 10000);
        Loader loader(pool);
        QObject::connect(&loader, &Loader::loadFinished, &loader,
                         [&](std::shared_ptr<Image>, const QString &, bool) { finished.fetch_add(1); });
        QObject::connect(&loader, &Loader::loadFailed, &loader,
                         [&](const QString &) { failed.fetch_add(1); });
        const QString path = fixtureDir + "/never-decoded.png";
        loader.loadAsync(path, loadPolicy());
        QCOMPARE(loader.isBusy(), true);
        QCOMPARE(loader.isLoading(path), true);

        loader.clearTasks();
        QCOMPARE(loader.isBusy(), false);
        QCOMPARE(loader.isLoading(path), false);

        gate.store(true);
        pool.waitForAll();
        QCOMPARE(finished.load(), 0);
        QCOMPARE(failed.load(), 0);
    }

    // The checkpoint contract's other half: a thumbnail job canceled before
    // its decode still produces the report object taskEnd dereferences — an
    // empty QImage becomes the error thumbnail, never a null Thumbnail, and a
    // canceled decode never arrives dressed up as a real decode result.
    void canceledThumbnailJobStillProducesAWellFormedReport() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/canceled.png", "png", QSize(4, 4)));
        LoadPolicy policy = loadPolicy();
        CancelToken token;
        token.request();
        auto thumbnail = ThumbnailerRunnable::generate(
            nullptr, fixtureDir + "/canceled.png", 64, /*crop=*/false, /*force=*/true, policy, &token);
        QVERIFY(thumbnail != nullptr);
        QCOMPARE(thumbnail->size(), 64);
        QVERIFY2(thumbnail->info() == QStringLiteral("error"),
                 "a canceled decode must arrive as the error thumbnail, not as a decode result");
    }

    // waitForProcess is the seam that turns a canceled frame extraction into a
    // free worker: slice-polled, kill + reap on the mark, instead of one
    // waitForFinished call that would hold the worker for the whole timeout.
    // Pinned against real processes, the way the header promises.
    void waitForProcessKillsARealProcessOnTheMark() {
        CancelToken token;
        token.request();
        QProcess process;
        process.start("sleep", {"30"});
        QVERIFY(process.waitForStarted(2000));
        QElapsedTimer t;
        t.start();
        ThumbnailerRunnable::waitForProcess(process, &token, 8000);
        QVERIFY2(t.elapsed() < 4000, "a marked wait held the worker for the full timeout");
        QCOMPARE(process.state(), QProcess::NotRunning);
    }

    void waitForProcessWithoutATokenWaitsInTheOneCall() {
        QProcess process;
        process.start("true");
        QVERIFY(process.waitForStarted(2000));
        ThumbnailerRunnable::waitForProcess(process, nullptr, 8000);
        QCOMPARE(process.state(), QProcess::NotRunning);
    }

    // One frame file per concurrent extraction: the same clip at two sizes (the
    // grid and the strip each run their own thumbnailer) used to hand two mpv
    // writers one path, and the reader that lost the race cached a torn frame
    // under its own correct id. The pid is the cross-instance face of the same
    // race; basename stays for a crashed run's leftovers.
    void videoFrameTempPathSeparatesConcurrentExtractions() {
        QTemporaryDir own;
        QVERIFY(own.isValid());
        const QString dir = own.filePath("tmp") + "/";
        const QString clip = "/some/where/clip.mp4";
        const QString base = ThumbnailerRunnable::videoFrameTempPath(dir, clip, 120, false);
        QCOMPARE(base, ThumbnailerRunnable::videoFrameTempPath(dir, clip, 120, false));
        QVERIFY(base != ThumbnailerRunnable::videoFrameTempPath(dir, clip, 240, false));
        QVERIFY(base != ThumbnailerRunnable::videoFrameTempPath(dir, clip, 120, true));
        QVERIFY(base != ThumbnailerRunnable::videoFrameTempPath(dir, "/other/clip.mp4", 120, false));
        QVERIFY(base.startsWith(dir));
        QVERIFY(base.contains(QStringLiteral("clip.mp4-")));
        QVERIFY2(base.contains(QString::number(QCoreApplication::applicationPid())),
                 "the frame file name must be process-unique");
    }

    // ShortcutBuilder had no coverage at all; these pin the event-to-sequence
    // mapping against the hardcoded default input map (Ctrl, WheelUp/Down,
    // the button names, and the double-click / RMB-release quirks).
    void shortcutBuilderMapsLettersAndModifiers() {
        inputMap = InputMap::getInstance();
        QKeyEvent xKey(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QString("x"));
        QCOMPARE(ShortcutBuilder::fromEvent(&xKey), QStringLiteral("X"));
        QKeyEvent ctrlX(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier, QString("x"));
        QCOMPARE(ShortcutBuilder::fromEvent(&ctrlX), QStringLiteral("Ctrl+X"));
        // a modifier alone is not a shortcut
        QKeyEvent ctrlOnly(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
        QCOMPARE(ShortcutBuilder::fromEvent(&ctrlOnly), QString());
    }

    void shortcutBuilderMapsWheelDirection() {
        inputMap = InputMap::getInstance();
        QWheelEvent down(QPointF(8, 8), QPointF(8, 8), QPoint(), QPoint(0, -120),
                         Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCOMPARE(ShortcutBuilder::fromEvent(&down), QStringLiteral("WheelDown"));
        QWheelEvent up(QPointF(8, 8), QPointF(8, 8), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCOMPARE(ShortcutBuilder::fromEvent(&up), QStringLiteral("WheelUp"));
    }

    void shortcutBuilderMapsMouseButtonsWithTheirQuirks() {
        inputMap = InputMap::getInstance();
        QMouseEvent lmb(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
        QCOMPARE(ShortcutBuilder::fromEvent(&lmb), QStringLiteral("LMB"));
        QMouseEvent rmbRelease(QEvent::MouseButtonRelease, QPointF(1, 1), QPointF(1, 1), Qt::RightButton,
                               Qt::RightButton, Qt::NoModifier);
        QCOMPARE(ShortcutBuilder::fromEvent(&rmbRelease), QStringLiteral("RMB"));
        // RMB is recognized on release only — a press is deliberately ignored
        QMouseEvent rmbPress(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1), Qt::RightButton,
                             Qt::RightButton, Qt::NoModifier);
        QCOMPARE(ShortcutBuilder::fromEvent(&rmbPress), QString());
        QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
        QCOMPARE(ShortcutBuilder::fromEvent(&dbl), QStringLiteral("LMB_DoubleClick"));
    }

    // ImageAnimated had no coverage at all: the QMovie-based loader and the
    // direct frame decode behind getImage() now run against a real two-frame
    // GIF.
    void imageAnimatedLoadsFramesFromAGif() {
        resetFixture();
        // minimal two-frame GIF89a (PIL-generated at authoring time)
        const QByteArray gif = QByteArray::fromBase64(
            "R0lGODlhAgACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACw"
            "AAAAAAgACAAAIBgABCAQQEAAh+QQBCgABACwAAAAAAgACAIEAAP8AAAAAAAAAAAAIBgABCAQ"
            "QEAA7");
        QFile f(fixtureDir + "/anim.gif");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(gif);
        f.close();

        auto image = *ImageFactory::createImage(fixtureDir + "/anim.gif", false, loadPolicy());
        QCOMPARE(image->type(), DocumentType::ANIMATED);
        auto animated = std::dynamic_pointer_cast<ImageAnimated>(image);
        QVERIFY(animated != nullptr);
        QVERIFY(animated->frameCount() >= 1);
        QVERIFY(animated->getImage() != nullptr);
        QVERIFY(!animated->getImage()->isNull());
    }

    // ThemeStore resolves every scheme with a valid tid and derived contrast
    // variants; the base colors are the table defaults verbatim.
    void themeStoreSchemesResolveTheirBaseColors() {
        auto black = ThemeStore::colorScheme(COLORS_BLACK);
        QCOMPARE(black.tid, static_cast<int>(COLORS_BLACK));
        QCOMPARE(black.background, QColor(QString::fromUtf8("#000000")));
        QCOMPARE(black.text, QColor(QString::fromUtf8("#b0b0b0")));
        QVERIFY(black.text_hc.isValid() && black.text_hc != black.text);
        QVERIFY(black.button.isValid());

        auto light = ThemeStore::colorScheme(COLORS_LIGHT);
        QCOMPARE(light.tid, static_cast<int>(COLORS_LIGHT));
        QCOMPARE(light.folderview_topbar, QColor(QString::fromUtf8("#ffffff")));
        QVERIFY(light.text_hc.isValid() && light.text_hc != light.text);

        auto darkblue = ThemeStore::colorScheme(COLORS_DARKBLUE);
        QCOMPARE(darkblue.background, QColor(QString::fromUtf8("#18191a")));
        QCOMPARE(darkblue.accent, QColor(QString::fromUtf8("#336ca5")));

        // the system scheme rides the platform palette, but the tid is its own
        auto system = ThemeStore::colorScheme(COLORS_SYSTEM);
        QCOMPARE(system.tid, static_cast<int>(COLORS_SYSTEM));
        QVERIFY(system.widget.isValid());
    }

    // DocumentInfo's exif path: a written-EXIF fixture flows through exiv2 into
    // the orientation accessor and the tag map
    void documentInfoReadsExifOrientationAndTags() {
        resetFixture();
        // orientation 6 = rotate 90 CW; Make/Model/DateTime feed loadExifTags
        // (PIL-written at authoring time; one long literal, correctness over beauty)
        const QByteArray exifBytes = QByteArray::fromBase64(
            "/9j/4AAQSkZJRgABAQAAAQABAAD/4QCwRXhpZgAATU0AKgAAAAgABQEPAAIAAAAJAAAASgEQAAIAAAAKAAAAVAESAAMAAAABAAYAAAEyAAIAAAAUAAAAXodpAAQAAAABAAAAcgAAAABUZXN0TWFrZQAAVGVzdE1vZGVsADIwMjQ6MDE6MDIgMDM6MDQ6MDUAAASCmgADAAAAAgABADyCnQADAAAAAgAcAAqIJwADAAAAAQBkAACSCgADAAAAAglgAGQAAAAA/9sAQwAIBgYHBgUIBwcHCQkICgwUDQwLCwwZEhMPFB0aHx4dGhwcICQuJyAiLCMcHCg3KSwwMTQ0NB8nOT04MjwuMzQy/9sAQwEJCQkMCwwYDQ0YMiEcITIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIy/8AAEQgACAAIAwEiAAIRAQMRAf/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMCBAMFBQQEAAABfQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/EAB8BAAMBAQEBAQEBAQEAAAAAAAABAgMEBQYHCAkKC//EALURAAIBAgQEAwQHBQQEAAECdwABAgMRBAUhMQYSQVEHYXETIjKBCBRCkaGxwQkjM1LwFWJy0QoWJDThJfEXGBkaJicoKSo1Njc4OTpDREVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoKDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uLj5OXm5+jp6vLz9PX29/j5+v/aAAwDAQACEQMRAD8Ahooor4c/OD//2Q==");
        QFile out(fixtureDir + "/exif.jpg");
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(exifBytes);
        out.close();

        DocumentInfo doc(fixtureDir + "/exif.jpg", LoadPolicy::fromSettings());
        // exifOrientation() returns QImageReader::transformation()'s flag
        // value, not the raw tag: orientation 6 lands as TransformationRotate90
        QCOMPARE(doc.exifOrientation(), 4); // QImageIOHandler::TransformationRotate90
        doc.loadExifTags(); // the tag map is lazy
        QVERIFY(!doc.getExifTags().isEmpty());
        // the rational tags' exact strings are exiv2's formatting domain —
        // pinned here: the keys exist and the verbatim strings survive
        QVERIFY(doc.getExifTags().contains(QStringLiteral("ExposureTime")));
        QVERIFY(doc.getExifTags().contains(QStringLiteral("F Number")));
        QVERIFY(doc.getExifTags().contains(QStringLiteral("Focal Length")));
        QCOMPARE(doc.getExifTags().value(QStringLiteral("ISO Speed ratings")), QStringLiteral("100"));
        QCOMPARE(doc.getExifTags().value(QStringLiteral("Make")), QStringLiteral("TestMake"));
        QCOMPARE(doc.getExifTags().value(QStringLiteral("Model")), QStringLiteral("TestModel"));
        QCOMPARE(doc.getExifTags().value(QStringLiteral("Date/Time")), QStringLiteral("2024:01:02 03:04:05"));
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
        auto image = *ImageFactory::createImage(fixtureDir + "/real.png", false, loadPolicy());
        QVERIFY(image->getImage()->size() == orig);

        Cache cache;
        WorkerPool pool(2);
        Scaler scaler(&cache, pool);
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

    // The equality relation Scaler::startRequest guards its buffer with, pinned
    // field by field: both requests differ only in the setting the worker reads
    // to choose NEAREST-versus-filter scaling, so they are different work and
    // must not be interchangeable. path is deliberately on the "does not change
    // the result" side — it identifies the file the result is for.
    void scalerRequestEqualityCoversEverythingThatChangesTheResult() {
        resetFixture();
        const QSize orig(64, 64);
        QVERIFY(writeBigImage(fixtureDir + "/real.png", "png", orig));
        auto image = *ImageFactory::createImage(fixtureDir + "/real.png", false, loadPolicy());
        const QString p = fixtureDir + "/real.png";

        ScalerRequest base(image, QSize(32, 32), p, QI_FILTER_BILINEAR, true);
        QVERIFY2(!(base == ScalerRequest(image, QSize(32, 32), p, QI_FILTER_BILINEAR, false)),
                 "requests that scale differently compare equal: the buffered one would be dropped");
        QVERIFY(!(base == ScalerRequest(image, QSize(40, 32), p, QI_FILTER_BILINEAR, true)));  // size
        QVERIFY(!(base == ScalerRequest(image, QSize(32, 32), p, QI_FILTER_NEAREST, true)));   // filter
        auto other = *ImageFactory::createImage(fixtureDir + "/real.png", false, loadPolicy());
        QVERIFY(!(base == ScalerRequest(other, QSize(32, 32), p, QI_FILTER_BILINEAR, true)));   // image
        QVERIFY(base == ScalerRequest(image, QSize(32, 32), fixtureDir + "/other.png",
                                      QI_FILTER_BILINEAR, true));                               // path only
    }

    void threeSynchronousRequestsHandOffExactlyTwice() {
        // The Scaler decides "a task is in flight" at the moment it hands the
        // request to the pool. It used to learn it from the pool instead, through
        // a DirectConnection that set `running` on the worker thread — so until
        // that thread woke up, every request looked like the first one, and each
        // of them re-used the single ScalerRunnable object and overwrote the
        // request of the task already queued. Those extra hand-offs all happen
        // synchronously, before this test returns control to the event loop, so
        // what is delivered is deterministic: the buffered result, once.
        resetFixture();
        const QSize orig(64, 64);
        QVERIFY(writeBigImage(fixtureDir + "/a.png", "png", orig));
        QVERIFY(writeBigImage(fixtureDir + "/b.png", "png", orig));
        auto a = *ImageFactory::createImage(fixtureDir + "/a.png", false, loadPolicy());
        auto b = *ImageFactory::createImage(fixtureDir + "/b.png", false, loadPolicy());

        Cache cache;
        cache.insert(a);
        cache.insert(b);
        WorkerPool pool(2);
        Scaler scaler(&cache, pool);
        int delivered = 0;
        QSize gotSize;
        QString gotPath;
        QObject::connect(&scaler, &Scaler::scalingFinished,
                         [&](QPixmap pix, ScalerRequest req) {
            ++delivered;
            gotSize = pix.size();
            gotPath = req.path;
        });

        scaler.requestScaled(ScalerRequest(a, QSize(32, 32), fixtureDir + "/a.png",
                                           QI_FILTER_BILINEAR, true));
        scaler.requestScaled(ScalerRequest(b, QSize(40, 20), fixtureDir + "/b.png",
                                           QI_FILTER_BILINEAR, true));
        scaler.requestScaled(ScalerRequest(b, QSize(48, 24), fixtureDir + "/b.png",
                                           QI_FILTER_BILINEAR, true));

        QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, 10000);
        QCOMPARE(gotSize, QSize(48, 24));   // the last request, not the first
        QCOMPARE(gotPath, fixtureDir + "/b.png");
        // the middle request (40x20) must never have been scaled and delivered:
        // every hand-off is synchronous above, so nothing can arrive later
        QTest::qWait(300);
        QCOMPARE(delivered, 1);
    }

    void aPinnedImageIsSkippedByEvictionAndReleasedLater() {
        // The scaler pins the image it is scaling so eviction cannot take it away
        // mid-flight. The pin was keyed on the basename while the map is keyed on
        // the absolute path, so it never pinned anything (and every miss returned
        // false into a discarded return value). Eviction also used to WAIT on the
        // pin — with the scaler's result now delivered through a queued
        // connection, waiting on a permit only this thread can release is a
        // deadlock, so eviction skips instead of blocking.
        resetFixture();
        const QSize orig(64, 64);
        QVERIFY(writeBigImage(fixtureDir + "/a.png", "png", orig));
        QVERIFY(writeBigImage(fixtureDir + "/b.png", "png", orig));
        auto a = *ImageFactory::createImage(fixtureDir + "/a.png", false, loadPolicy());
        auto b = *ImageFactory::createImage(fixtureDir + "/b.png", false, loadPolicy());

        Cache cache;
        QVERIFY(cache.insert(a));
        QVERIFY(cache.insert(b));
        WorkerPool pool(2);
        Scaler scaler(&cache, pool);
        int delivered = 0;
        QObject::connect(&scaler, &Scaler::scalingFinished, [&](QPixmap, ScalerRequest) {
            ++delivered;
        });

        scaler.requestScaled(ScalerRequest(a, QSize(32, 32), fixtureDir + "/a.png",
                                           QI_FILTER_BILINEAR, true));

        cache.trimTo(QStringList{});
        QVERIFY2(cache.contains(a->filePath()),
                 "the image being scaled was evicted under the scaler — the pin missed its key");
        QVERIFY(!cache.contains(b->filePath())); // not pinned, not listed: gone

        QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, 10000);
        cache.trimTo(QStringList{});
        QVERIFY(!cache.contains(a->filePath())); // unlocked by now, so this pass takes it
    }

    // Ownership used to be the bug here: the runnable was the one object the
    // Scaler created and nobody owned (autoDelete(false), no destructor), so
    // every Scaler that ever scaled something left it behind, still holding
    // its last ScalerRequest and through it the decoded image. The pool owns
    // its jobs outright now — submit() transfers, run() returning is the
    // pool's deletion point on the worker thread — and this pin stays to
    // prove the Scaler leaves nothing behind: once it is dead, the image's
    // refcount is back to the caller's copy alone.
    void destroyingTheScalerReleasesTheImageItWasHolding() {
        resetFixture();
        const QSize orig(64, 64);
        QVERIFY(writeBigImage(fixtureDir + "/real.png", "png", orig));
        auto image = *ImageFactory::createImage(fixtureDir + "/real.png", false, loadPolicy());
        QCOMPARE(image.use_count(), 1L);

        {
            Cache cache;
            QVERIFY(cache.insert(image));
            WorkerPool pool(2);
            Scaler scaler(&cache, pool);
            int delivered = 0;
            QObject::connect(&scaler, &Scaler::scalingFinished, [&](QPixmap, ScalerRequest) {
                ++delivered;
            });
            scaler.requestScaled(ScalerRequest(image, QSize(32, 32), fixtureDir + "/real.png",
                                               QI_FILTER_BILINEAR, true));
            QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, 10000);
        }

        // the result travels through a queued hop, which copies the request into
        // a posted event; give that a chance to settle before blaming a holder
        QTest::qWait(100);
        QVERIFY2(image.use_count() == 1,
                 "a destroyed Scaler still holds its last request, and the image inside it");
    }

    void videoFrameTempFilesDoNotCollideAcrossDirectories() {
        // Two clips that share a basename in different folders used to be handed
        // the same temp file in the shared tmpDir: one worker read and removed the
        // other's frame, then cached it under its own correct id — a wrong
        // thumbnail that stays wrong. The name has to be per source path, and
        // stable for one source because the writer deletes it after use.
        const QString a = ThumbnailerRunnable::videoFrameTempPath("/tmp/t/", "/videos/x/clip.mp4", 120, false);
        const QString b = ThumbnailerRunnable::videoFrameTempPath("/tmp/t/", "/videos/y/clip.mp4", 120, false);
        QVERIFY2(a != b, "same basename in different folders shared one temp file");
        QCOMPARE(a, ThumbnailerRunnable::videoFrameTempPath("/tmp/t/", "/videos/x/clip.mp4", 120, false));
        QVERIFY(a.startsWith("/tmp/t/clip.mp4-"));
        // a source that differs by more than its directory still differs
        QVERIFY(a != ThumbnailerRunnable::videoFrameTempPath("/tmp/t/", "/videos/x/clip.mkv", 120, false));
    }

    void thumbnailerAccountingSurvivesAClearMidFlight() {
        resetFixture();
        WorkerPool pool(2);
        Thumbnailer thumb(pool, EngineBridge::instance());
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

    // The presenter-level shape of the shared instance, pinned at the unit the
    // presenters share: one wave's clearTasks kills the other wave's
    // still-pending requests (the engine's pending queue is process-wide,
    // which predates the shared instance), and the recovery is the next wave
    // re-requesting the same files. This pins the recovery: every file of a
    // killed batch arrives after the re-request, whichever fate its first job
    // met — dispatched and delivered, canceled before its checkpoint and
    // re-requested fresh, or canceled inside generate and delivered by the
    // survivor. Narrowing the kill window needs engine batch ids; while this
    // recovery holds, that protocol change buys nothing worth its pin tax.
    void aClearedBatchIsRecoveredByTheNextWave() {
        resetFixture();
        WorkerPool pool(2);
        Thumbnailer thumb(pool, EngineBridge::instance());
        QSet<QString> ready;
        QObject::connect(&thumb, &Thumbnailer::thumbnailReady, &thumb,
                         [&ready](std::shared_ptr<Thumbnail>, const QString &path) {
                             ready.insert(path);
                         });
        const QStringList batch = {
            fixtureDir + "/img1.jpg",
            fixtureDir + "/img2.jpg",
            fixtureDir + "/img10.jpg",
        };
        for(const QString &path : batch)
            thumb.getThumbnailAsync(path, 32, false, false);
        // three requests, two in flight: the clear provably kills a pending
        // request and marks jobs that may be inside generate()
        thumb.clearTasks();

        // the next wave re-requests the same files, and the batch recovers
        for(const QString &path : batch)
            thumb.getThumbnailAsync(path, 32, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), batch.count(), 10000);
        thumb.waitForDone();
    }
};

#include "machinery_tests.moc"

int runMachineryTests(int argc, char **argv) {
    TestMachinery test;
    return runIfOwned(test, argc, argv);
}
