#pragma once

#include "components/directorymanager/enginebridge.h"
#include "components/thumbnailer/thumbnailerrunnable.h"
#include "components/cache/thumbnailcache.h"
#include "components/workerpool/workerpool.h"
#include "settings.h"

class Thumbnailer : public QObject
{
    Q_OBJECT
public:
    // The pool and the bridge are required, not defaulted: production names
    // the shared pool and the process-wide bridge at the construction site,
    // tests name their isolated ones. A default lets every call site keep the
    // hidden choice (00cf517e's rule).
    explicit Thumbnailer(WorkerPool &_pool, EngineBridge &bridge);
    ~Thumbnailer() override;
    static std::shared_ptr<Thumbnail> getThumbnail(QString filePath, int size);
    void clearTasks();
    void waitForDone();

public slots:
    void getThumbnailAsync(const QString& path, int size, bool crop, bool force);

private:
    EngineBridge &mBridge;
    WorkerPool &pool;
    ThumbnailCache *cache;
    void startThumbnailerThread(QString filePath, int size, bool crop, bool force);
    QMultiMap<QString, int> runningTasks;
    // engine-dispatched requests (path -> {size, crop}); cleared entries are
    // reported as done so the scheduler's in-flight accounting doesn't leak
    QMultiHash<QString, QPair<int, bool>> dispatched;
    QHash<QString, bool> mPendingForce;
    bool mBatchStart = true;

private slots:
    void onTaskStart(const QString& filePath, int size);
    void onTaskEnd(const std::shared_ptr<Thumbnail>& thumbnail, const QString& filePath, bool crop);
    void onThumbNext(const QString& filePath, int size, bool crop);

signals:
    void thumbnailReady(std::shared_ptr<Thumbnail> thumbnail, QString filePath);
};
