#include "workerpool.h"

WorkerPool::WorkerPool(int threadCount) {
    if(threadCount < 1)
        threadCount = 1;
    for(int i = 0; i < threadCount; ++i)
        workers.emplace_back([this] { workerLoop(); });
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    workAvailable.notify_all();
    for(std::thread &w : workers)
        w.join();
    // workers are gone, so nothing left in the queue ever ran; the pool exits
    // empty-handed rather than holding jobs whose owner may already be gone
    for(Entry &e : queue)
        e.job.reset();
    idle.notify_all();
}

void WorkerPool::submit(PoolOwner owner, int priority, std::unique_ptr<PoolJob> job) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(Entry{owner, priority, nextSeq++, std::move(job)});
    }
    workAvailable.notify_one();
}

std::vector<std::unique_ptr<PoolJob>> WorkerPool::drain(PoolOwner owner) {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::unique_ptr<PoolJob>> taken;
    for(auto it = queue.begin(); it != queue.end();) {
        if(it->owner == owner) {
            taken.push_back(std::move(it->job));
            it = queue.erase(it);
        } else {
            ++it;
        }
    }
    return taken;
}

void WorkerPool::cancel(PoolOwner owner) {
    std::lock_guard<std::mutex> lock(mutex);
    for(const Entry &e : queue)
        if(e.owner == owner)
            e.job->requestCancel();
    for(const Running &r : running)
        if(r.owner == owner)
            r.job->requestCancel();
    // no notify: cancel() changes no state any waiter is waiting on. A caller
    // that wants to know the marked jobs are gone calls waitForOwner().
}

void WorkerPool::waitForOwner(PoolOwner owner) {
    std::unique_lock<std::mutex> lock(mutex);
    // the wait releases the lock while blocked — no state lock spans a wait
    idle.wait(lock, [this, owner] { return ownerIdle(owner); });
}

void WorkerPool::waitForAll() {
    std::unique_lock<std::mutex> lock(mutex);
    idle.wait(lock, [this] { return allIdle(); });
}

void WorkerPool::workerLoop() {
    std::unique_lock<std::mutex> lock(mutex);
    for(;;) {
        workAvailable.wait(lock, [this] { return stopping || !queue.empty(); });
        if(stopping)
            return;
        // highest priority wins, ties go to the older submission
        auto best = queue.begin();
        for(auto it = std::next(queue.begin()); it != queue.end(); ++it)
            if(it->priority > best->priority
               || (it->priority == best->priority && it->seq < best->seq))
                best = it;
        Entry entry = std::move(*best);
        queue.erase(best);
        PoolJob *job = entry.job.get();
        running.push_back(Running{entry.owner, job});
        lock.unlock();       // the lock never spans run()
        job->run();          // queued signal delivery needs nothing from us
        // Retire the record and the job in that order, with the record going
        // under the lock: cancel() reaches a job only through a record and only
        // while it holds this mutex, so a live record always names a live job
        // and a job that is about to die has nothing left that can mark it.
        lock.lock();
        for(auto it = running.begin(); it != running.end(); ++it) {
            if(it->job == job) {
                running.erase(it);
                break;
            }
        }
        idle.notify_all();
        lock.unlock();
        entry.job.reset();   // the pool is the single deleter
        lock.lock();
    }
}

bool WorkerPool::ownerIdle(PoolOwner owner) {
    for(const Running &r : running)
        if(r.owner == owner)
            return false;
    for(const Entry &e : queue)
        if(e.owner == owner)
            return false;
    return true;
}

bool WorkerPool::allIdle() {
    return running.empty() && queue.empty();
}
