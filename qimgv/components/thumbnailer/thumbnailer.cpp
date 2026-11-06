#include "thumbnailer.h"

#include <utility>

#include <QTimer>

#include "../directorymanager/enginebridge.h"

Thumbnailer::Thumbnailer(WorkerPool &_pool, EngineBridge &bridge)
    : mBridge(bridge),
      pool(_pool)
{
    connect(&mBridge, &EngineBridge::thumbNext,
            this, &Thumbnailer::onThumbNext);
    cache = new ThumbnailCache(settings->thumbnailCacheDir());
    // the shared pool's size is decided once, where it is constructed (Core)
    // startup sweep, after the first frames have settled; stat-only walk
    QTimer::singleShot(3000, this, [this]() {
        cache->purgeOlderThan(ThumbnailCache::cacheMaxIdleDays,
                              ThumbnailCache::cacheMaxBytes);
    });
}

Thumbnailer::~Thumbnailer() {
    auto dropped = pool.drain(PoolOwner::Thumbnailer);
    // A frame extraction in progress would otherwise be waited out for its whole
    // 8 s timeout while the app sits in a destructor. A job stopped by that mark
    // leaves its request in the engine's in-flight set, which is moot here: the
    // bridge that holds it belongs to a process that is already going away.
    pool.cancel(PoolOwner::Thumbnailer);
    pool.waitForOwner(PoolOwner::Thumbnailer);
    // a parentless QObject: nothing else in the graph owns it, and the jobs
    // that were handed its pointer are gone with the drain and the wait above
    delete cache;
}

void Thumbnailer::waitForDone() {
    pool.waitForOwner(PoolOwner::Thumbnailer);
}

void Thumbnailer::clearTasks() {
    // drain() kills queued jobs whose taskEnd will never fire — report every
    // dispatched request as done so the engine's in-flight accounting doesn't
    // leak, then drop the pending batch
    auto &bridge = mBridge;
    for (auto it = dispatched.begin(); it != dispatched.end(); ++it)
        bridge.thumbDone(it.key(), it.value().first, it.value().second);
    dispatched.clear();
    bridge.thumbClear();
    auto dropped = pool.drain(PoolOwner::Thumbnailer);
    // The queued half is gone with the drain; the running half is a frame
    // extraction that will otherwise occupy a worker for up to its 8 s timeout
    // and then report a picture nobody asked for any more. Marking it costs one
    // store and frees the worker within a poll slice, and a job that had already
    // announced itself still reports, so dispatched, runningTasks and the
    // engine's count stay balanced without anything here knowing which jobs
    // were marked.
    pool.cancel(PoolOwner::Thumbnailer);
    mBatchStart = true;
}

std::shared_ptr<Thumbnail> Thumbnailer::getThumbnail(QString filePath, int size) {
    return ThumbnailerRunnable::generate(nullptr, std::move(filePath), size, false, false, LoadPolicy::fromSettings());
}

void Thumbnailer::getThumbnailAsync(const QString& path, int size, bool crop, bool force) {
    const QString key = path + QString("|%1|%2").arg(size).arg(crop);
    if(runningTasks.contains(path, size) || dispatched.contains(path, qMakePair(size, crop)))
        return;
    // viewport priority lives in the engine; the runnable starts on ThumbNext.
    // `force` (cache bypass) is C++-side state — it only matters at decode time.
    mPendingForce.insert(key, force);
    mBridge.thumbRequest(path, size, crop, mBatchStart);
    mBatchStart = false;
    dispatched.insert(path, qMakePair(size, crop));
}

void Thumbnailer::startThumbnailerThread(QString filePath, int size, bool crop, bool force) {
    auto job = std::make_unique<ThumbnailerRunnable>(settings->useThumbnailCache() ? cache : nullptr,
                                                     std::move(filePath), size, crop, force,
                                                     LoadPolicy::fromSettings());
    connect(job.get(), &ThumbnailerRunnable::taskStart, this, &Thumbnailer::onTaskStart);
    connect(job.get(), &ThumbnailerRunnable::taskEnd, this, &Thumbnailer::onTaskEnd);
    pool.submit(PoolOwner::Thumbnailer, 0, std::move(job));
}

void Thumbnailer::onTaskStart(const QString& filePath, int size) {
    runningTasks.insert(filePath, size);
}

void Thumbnailer::onTaskEnd(const std::shared_ptr<Thumbnail>& thumbnail, const QString& filePath, bool crop) {
    runningTasks.remove(filePath, thumbnail->size());
    dispatched.remove(filePath, qMakePair(thumbnail->size(), crop));
    mBridge.thumbDone(filePath, thumbnail->size(), crop);
    emit thumbnailReady(thumbnail, filePath);
}

void Thumbnailer::onThumbNext(const QString& filePath, int size, bool crop) {
    const QString key = filePath + QString("|%1|%2").arg(size).arg(crop);
    const bool force = mPendingForce.take(key);
    startThumbnailerThread(filePath, size, crop, force);
}
