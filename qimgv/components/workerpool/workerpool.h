#pragma once

#include <QtGlobal>
#include <array>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The one executor the image pipeline runs on: Loader decodes, Scaler scales,
// Thumbnailer generates. Replaces three per-component QThreadPool instances,
// whose waitForDone() tore the pool down and rebuilt it on every wave — the
// shape that trips TSan's thread-registry CHECK, because Qt 6.10 joins pool
// threads via pthread_clockjoin_np and the sanitizer does not intercept that
// call (see the QIMGV_SANITIZERS block in the root CMakeLists.txt, and
// ~/tsan-qthreadpool-check/ for the minimal repro). Workers are created in the
// constructor and joined in the destructor; in between, the pool never creates
// or joins a thread.
//
// Ownership is explicit and single:
//   - submit() transfers the job to the pool. The pool destroys it on the
//     worker thread right after run() returns — the same pattern Thumbnailer's
//     autoDelete(true) runnables already relied on. The jobs are parentless,
//     childless, timerless QObjects whose signals carry copies of their
//     arguments, so queued delivery survives the sender.
//   - drain() hands the queued-but-unstarted jobs of one owner back to the
//     caller, which decides whether to delete or re-submit them.
// Nobody else ever deletes a submitted job.
//
// Priorities: Loader's tier-1 decode runs at 2, Scaler at 1, Loader's tier-2
// upgrade and Thumbnailer at 0. This is a deliberate consequence of sharing
// one pool: an active scaling now outranks a background full-res upgrade,
// where the per-pool isolation of the QThreadPool era never let them compete.

enum class PoolOwner : quint8 { Loader, Scaler, Thumbnailer };

class PoolJob {
public:
    virtual ~PoolJob() = default;
    // runs on a worker thread; never dereferences the settings global —
    // per-request state travels inside the job (LoadPolicy, ScalerRequest)
    virtual void run() = 0;
};

class WorkerPool {
public:
    explicit WorkerPool(int threadCount);
    ~WorkerPool();
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    void submit(PoolOwner owner, int priority, std::unique_ptr<PoolJob> job);
    // returns everything of `owner` that is queued but not started; running
    // jobs are not interrupted and complete normally
    std::vector<std::unique_ptr<PoolJob>> drain(PoolOwner owner);
    // blocks until `owner` has nothing queued and nothing inside run()
    void waitForOwner(PoolOwner owner);
    void waitForAll();

private:
    struct Entry {
        PoolOwner owner;
        int priority;
        unsigned long seq; // FIFO within one priority
        std::unique_ptr<PoolJob> job;
    };

    void workerLoop();
    bool ownerIdle(PoolOwner owner); // mutex held
    bool allIdle();                  // mutex held

    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable workAvailable; // workers wait for submissions
    std::condition_variable idle;          // waitForOwner/waitForAll wait here
    std::deque<Entry> queue;
    std::array<int, 3> active = {{0, 0, 0}}; // jobs inside run(), per owner
    unsigned long nextSeq = 0;
    bool stopping = false;
};
