#include <QTest>
#include <QThread>
#include <atomic>

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
};

namespace {

class CountingJob : public PoolJob {
public:
    CountingJob(std::atomic_int *runs, std::atomic_int *deaths)
        : runs(runs), deaths(deaths) {}
    ~CountingJob() override { deaths->fetch_add(1); }
    void run() override { runs->fetch_add(1); }
private:
    std::atomic_int *runs;
    std::atomic_int *deaths;
};

// occupies its worker until released, so the test controls what stays queued
class GatedJob : public PoolJob {
public:
    GatedJob(std::atomic_bool *gate, std::atomic_int *starts, std::atomic_int *deaths)
        : gate(gate), starts(starts), deaths(deaths) {}
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
    OrderJob(int marker, std::atomic_int *next, int *order)
        : marker(marker), next(next), order(order) {}
    void run() override { order[next->fetch_add(1)] = marker; }
private:
    int marker;
    std::atomic_int *next;
    int *order;
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

int runWorkerPoolTests(int argc, char **argv) {
    TestWorkerPool t;
    return runIfOwned(t, argc, argv);
}

#include "workerpool_tests.moc"
