#pragma once

#include <QThreadPool>
#include "components/directorymanager/enginebridge.h"
#include "components/thumbnailer/thumbnailerrunnable.h"
#include "components/cache/thumbnailcache.h"
#include "settings.h"

class Thumbnailer : public QObject
{
    Q_OBJECT
public:
    // The bridge is required, not defaulted: production names the process-wide
    // instance at the construction site, tests name their isolated one. A
    // default lets every call site keep the hidden choice (00cf517e's rule).
    explicit Thumbnailer(EngineBridge &bridge);
    ~Thumbnailer() override;
    static std::shared_ptr<Thumbnail> getThumbnail(QString filePath, int size);
    void clearTasks();
    void waitForDone();

public slots:
    void getThumbnailAsync(QString path, int size, bool crop, bool force);

private:
    EngineBridge &mBridge;
    ThumbnailCache *cache;
    QThreadPool *pool = nullptr;
    void startThumbnailerThread(QString filePath, int size, bool crop, bool force);
    QMultiMap<QString, int> runningTasks;
    // engine-dispatched requests (path -> {size, crop}); cleared entries are
    // reported as done so the scheduler's in-flight accounting doesn't leak
    QMultiHash<QString, QPair<int, bool>> dispatched;
    QHash<QString, bool> mPendingForce;
    bool mBatchStart = true;

private slots:
    void onTaskStart(QString filePath, int size);
    void onTaskEnd(std::shared_ptr<Thumbnail> thumbnail, QString filePath, bool crop);
    void onThumbNext(QString filePath, int size, bool crop);

signals:
    void thumbnailReady(std::shared_ptr<Thumbnail> thumbnail, QString filePath);
};
