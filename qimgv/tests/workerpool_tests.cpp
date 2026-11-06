#include <QTest>
#include <QThread>
#include <atomic>
#include <thread>

#include "engine_stack_fixture.h"
#include "components/workerpool/workerpool.h"

// The WorkerPool's own contracts, independent of any component using it:
// persistent workers (the TSan-tree pin for "our pools stop churning
// threads"), explicit ownership (drain returns unstarted jobs, the pool is
// the single deleter), priority order, and per-owner waiting.
class TestWorkerPool : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestWorkerPool() : EngineStackFixture("workerpool") {}

private slots:
    void initTestCase() { init(); }

    void persistentWorkersSurviveSubmitAndDrainWaves();
    void drainReturnsExactlyTheUnstartedJobs();
    void priorityOrderIsHonoredWithinASharedPool();
    void waitForOwnerWaitsOnlyForItsOwner();
    void waitForAllWaitsForEveryOwner();
    void cancelReachesQueuedJobs();
    void cancelReachesAJobInsideRun();
    void cancelSparesOtherOwners();
    void cancelDoesNotSatisfyWaiters();
};

namespace {

class CountingJob : public PoolJob {
public:
    CountingJob(std::atomic_int *_runs, std::atomic_int *_deaths)
        : runs(_runs), deaths(_deaths) {}
    ~CountingJob() override { deaths->fetch_add(1); }
    void run() override { runs->fetch_add(1); }
private:
    std::atomic_int *runs;
    std::atomic_int *deaths;
};

// occupies its worker until released, so the test controls what stays queued
class GatedJob : public PoolJob {
public:
    GatedJob(std::atomic_bool *_gate, std::atomic_int *_starts, std::atomic_int *_deaths)
        : gate(_gate), starts(_starts), deaths(_deaths) {}
    ~GatedJob() override { deaths->fetch_add(1); }
    void run() override {
        starts->fetch_add(1);
        while(!gate->load())
            QThread::msleep(1);
    }
private:
    std::atomic_bool *gate;
    std::atomic_int *starts;
    std::atomic_int *deaths;
};

class OrderJob : public PoolJob {
public:
    OrderJob(int _marker, std::atomic_int *_next, int *_order)
        : marker(_marker), next(_next), order(_order) {}
    void run() override { order[next->fetch_add(1)] = marker; }
private:
    int marker;
    std::atomic_int *next;
    int *order;
};

// records whether the cancel mark was already set when run() began: the
// queued half of cancel()'s contract — the mark travels with the job, so the
// entry checkpoint a consumer keeps sees it whenever the job eventually starts
class MarkedJob : public PoolJob {
public:
    MarkedJob(std::atomic_int *_sawMark, std::atomic_int *_deaths)
        : sawMark(_sawMark), deaths(_deaths) {}
    ~MarkedJob() override { deaths->fetch_add(1); }
    void run() override {
        if(cancelRequested())
            sawMark->fetch_add(1);
    }
private:
    std::atomic_int *sawMark;
    std::atomic_int *deaths;
};

// spins inside run() until the cancel mark arrives — the deterministic
// stand-in for "cancel() fired while this job was running"
class CancelGatedJob : public PoolJob {
public:
    CancelGatedJob(std::atomic_bool *_sawMark, std::atomic_int *_deaths)
        : sawMark(_sawMark), deaths(_deaths) {}
    ~CancelGatedJob() override { deaths->fetch_add(1); }
    void run() override {
        while(!cancelRequested())
            QThread::msleep(1);
        sawMark->store(true);
    }
private:
    std::atomic_bool *sawMark;
    std::atomic_int *deaths;
};

} // namespace

void TestWorkerPool::persistentWorkersSurviveSubmitAndDrainWaves() {
    // The TSan-tree pin for "our pools stop churning threads": QThreadPool's
    // waitForDone() joined every pool thread on every wave (through the
    // unintercepted pthread_clockjoin_np), so the next wave's pthread_create
    // reused a registry-live tid and CHECK-aborted — 40 waves were already
    // fatal (~/tsan-qthreadpool-check/). This pool creates its workers once
    // and joins them once, so the same wave shape must simply finish. The
    // regression here IS the sanitizer abort in build-tsan, not a failed
    // assert.
    std::atomic_int runs{0}, deaths{0};
    {
        WorkerPool pool(2);
        for(int wave = 0; wave < 400; ++wave) {
            pool.submit(PoolOwner::Loader, 0, std::make_unique<CountingJob>(&runs, &deaths));
            pool.waitForAll();
        }
    }
    QCOMPARE(runs.load(), 400);
    QCOMPARE(deaths.load(), 400);
}

void TestWorkerPool::drainReturnsExactlyTheUnstartedJobs() {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, runs{0}, deaths{0};
    {
        WorkerPool pool(1); // a single worker: the first job occupies it
        pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths));
        QTRY_COMPARE_WITH_TIMEOUT(starts.load(), 1, 10000); // worker is inside run()
        for(int i = 0; i < 3; ++i)
            pool.submit(PoolOwner::Loader, 0, std::make_unique<CountingJob>(&runs, &deaths));

        auto taken = pool.drain(PoolOwner::Loader);
        QVERIFY2(taken.size() == 3, "only the queued jobs come back, not the running one");
        QCOMPARE(runs.load(), 0);          // so none of them ever ran
        taken.clear();                     // caller deletes what it took back
        QCOMPARE(deaths.load(), 3);

        gate = true;                       // the running job finishes normally
        pool.waitForOwner(PoolOwner::Loader);
        QCOMPARE(runs.load(), 0);          // drained jobs never ran
        QCOMPARE(deaths.load(), 4);        // the gated job was pool-deleted
    }
    QCOMPARE(deaths.load(), 4);
}

void TestWorkerPool::priorityOrderIsHonoredWithinASharedPool() {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, next{0}, deaths{0};
    int order[3] = {0, 0, 0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Scaler, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths)); // occupies
    QTRY_COMPARE_WITH_TIMEOUT(starts.load(), 1, 10000);
    // submitted low first: priority, not submission order, decides
    pool.submit(PoolOwner::Loader, 0, std::make_unique<OrderJob>(0, &next, order));
    pool.submit(PoolOwner::Loader, 2, std::make_unique<OrderJob>(2, &next, order));
    pool.submit(PoolOwner::Scaler, 1, std::make_unique<OrderJob>(1, &next, order));
    gate = true;
    pool.waitForAll();
    QCOMPARE(order[0], 2);
    QCOMPARE(order[1], 1);
    QCOMPARE(order[2], 0);
}

void TestWorkerPool::waitForOwnerWaitsOnlyForItsOwner() {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, runs{0}, deaths{0};
    WorkerPool pool(2);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths));
    QTRY_COMPARE_WITH_TIMEOUT(starts.load(), 1, 10000);
    pool.submit(PoolOwner::Thumbnailer, 0, std::make_unique<CountingJob>(&runs, &deaths));
    // returns while Loader is still held: ownership scopes the wait
    pool.waitForOwner(PoolOwner::Thumbnailer);
    QCOMPARE(runs.load(), 1);
    QCOMPARE(starts.load(), 1);
    QVERIFY2(!gate.load(), "waitForOwner(Thumbnailer) must not wait out the Loader job");
    gate = true;
    pool.waitForOwner(PoolOwner::Loader);
    pool.waitForAll();
}

    // The settings bus drains the whole pool: Core::readSettings() opens with
    // waitForAll(), so the price of pressing Save is bounded by every owner's
    // queue, not by the pipeline the changed setting actually feeds. The
    // narrow form already has a test above; this pins the width of the wide
    // one, because that width is the part that runs on the UI thread.
void TestWorkerPool::waitForAllWaitsForEveryOwner() {
        std::atomic_bool gate{false};
        std::atomic_int loaderStarts{0}, thumbRuns{0}, deaths{0};
        WorkerPool pool(2);
        pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &loaderStarts, &deaths));
        QTRY_COMPARE_WITH_TIMEOUT(loaderStarts.load(), 1, 10000);
        pool.submit(PoolOwner::Thumbnailer, 0, std::make_unique<CountingJob>(&thumbRuns, &deaths));
        pool.waitForOwner(PoolOwner::Thumbnailer);
        QCOMPARE(thumbRuns.load(), 1);   // one owner is provably finished with...
        QVERIFY(loaderStarts.load() == 1); // ...while another is still inside a job

        std::atomic_bool returned{false};
        std::thread waiter([&pool, &returned]() {
            pool.waitForAll();
            returned.store(true);
        });
        QThread::msleep(100);
        QVERIFY2(!returned.load(), "waitForAll() returned while a Loader job was still running");
        gate = true;
        waiter.join();                 // it was waiting: opening the gate releases it
        QVERIFY(returned.load());
        // the pool destroys completed jobs on its worker thread, so the death
        // count catches up asynchronously — asserting it as an instant value is
        // a race this test authored, and it was the one red that arrived
        // unnamed in the ASan sweep
        QTRY_COMPARE_WITH_TIMEOUT(deaths.load(), 2, 10000);
    }

// cancel() marks; it never stops anything by itself. What the pool owes its
// callers is exactly three facts: the mark reaches queued jobs and jobs
// inside run(), it reaches nothing outside the owner it was given, and it
// satisfies no waiter — only a retire does. Everything on top (the entry
// checkpoint, the silent drop, the still-reports rule) is the consumer's
// half, pinned where the consumers live.
void TestWorkerPool::cancelReachesQueuedJobs() {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, marked{0}, deaths{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths));
    QTRY_COMPARE_WITH_TIMEOUT(starts.load(), 1, 10000);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<MarkedJob>(&marked, &deaths));
    pool.cancel(PoolOwner::Loader);
    gate = true;
    pool.waitForOwner(PoolOwner::Loader);
    QCOMPARE(marked.load(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(deaths.load(), 2, 10000);
}

void TestWorkerPool::cancelReachesAJobInsideRun() {
    std::atomic_bool sawMark{false};
    std::atomic_int deaths{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<CancelGatedJob>(&sawMark, &deaths));
    QThread::msleep(50); // it is inside run(), spinning on the mark
    pool.cancel(PoolOwner::Loader);
    // this is the waitForOwner the consumers' clearTasks/dtor pair leans on:
    // it can only return once the running record was retired, so a record
    // that leaked would hang here rather than pass
    pool.waitForOwner(PoolOwner::Loader);
    QCOMPARE(sawMark.load(), true);
    QTRY_COMPARE_WITH_TIMEOUT(deaths.load(), 1, 10000);
}

void TestWorkerPool::cancelSparesOtherOwners() {
    std::atomic_bool loaderMark{false}, scalerMark{false};
    std::atomic_int deaths{0};
    WorkerPool pool(2);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<CancelGatedJob>(&loaderMark, &deaths));
    pool.submit(PoolOwner::Scaler, 0, std::make_unique<CancelGatedJob>(&scalerMark, &deaths));
    QThread::msleep(50);
    pool.cancel(PoolOwner::Loader);
    QTRY_COMPARE_WITH_TIMEOUT(loaderMark.load(), true, 10000);
    QThread::msleep(100);
    QVERIFY2(!scalerMark.load(), "cancel(Loader) marked a job that belongs to Scaler");
    pool.cancel(PoolOwner::Scaler);
    pool.waitForAll();
    QCOMPARE(scalerMark.load(), true);
    QTRY_COMPARE_WITH_TIMEOUT(deaths.load(), 2, 10000);
}

void TestWorkerPool::cancelDoesNotSatisfyWaiters() {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, deaths{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths));
    QTRY_COMPARE_WITH_TIMEOUT(starts.load(), 1, 10000);
    std::atomic_bool returned{false};
    std::thread waiter([&pool, &returned]() {
        pool.waitForOwner(PoolOwner::Loader);
        returned.store(true);
    });
    QThread::msleep(100);
    QVERIFY(!returned.load()); // blocked on the running job, as designed
    pool.cancel(PoolOwner::Loader);
    QThread::msleep(100);
    QVERIFY2(!returned.load(), "cancel() satisfied a waiter without a retire");
    gate = true;
    waiter.join(); // the retire, not the cancel, is what released it
    QVERIFY(returned.load());
    QTRY_COMPARE_WITH_TIMEOUT(deaths.load(), 1, 10000);
}

int runWorkerPoolTests(int argc, char **argv) {
    TestWorkerPool t;
    return runIfOwned(t, argc, argv);
}

#include "workerpool_tests.moc"
