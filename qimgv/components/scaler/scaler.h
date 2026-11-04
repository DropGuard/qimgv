#pragma once

#include <QObject>
#include <QThreadPool>
#include <QThread>
#include "components/cache/cache.h"
#include "scalerrequest.h"
#include "scalerrunnable.h"

class Scaler : public QObject {
    Q_OBJECT
public:
    explicit Scaler(Cache *_cache, QObject *parent = nullptr);

signals:
    void scalingFinished(QPixmap result, const ScalerRequest &request);
    void acceptScalingResult(std::shared_ptr<const QImage> image, const ScalerRequest &req);

public slots:
    void requestScaled(const ScalerRequest &req);

private slots:
    // every slot here runs on this object's thread: the pool reports through a
    // queued connection, so the state machine below has exactly one owner
    void onTaskFinish(std::shared_ptr<const QImage> scaled, const ScalerRequest &req);
    void slotForwardScaledResult(std::shared_ptr<const QImage> image, const ScalerRequest &req);

private:
    QThreadPool *pool = nullptr;
    ScalerRunnable *runnable;
    bool buffered, running;
    ScalerRequest bufferedRequest, startedRequest;

    Cache *cache;

    // hands the request to the pool and records it as in flight — on this thread,
    // at the moment of hand-off; the pool telling us back would be both later
    // and racy, since maxThreadCount(1) lets a second hand-off overwrite the
    // first task's request while it is already running
    void startRequest(const ScalerRequest &req);
};
