#pragma once

#include <QObject>
#include <QThread>
#include "components/cache/cache.h"
#include "components/workerpool/workerpool.h"
#include "scalerrequest.h"
#include "scalerrunnable.h"

class Scaler : public QObject {
    Q_OBJECT
public:
    explicit Scaler(Cache *_cache, WorkerPool &_pool, QObject *parent = nullptr);
    ~Scaler() override;

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
    WorkerPool &pool;
    bool buffered, running;
    ScalerRequest bufferedRequest, startedRequest;

    Cache *cache;

    // hands the request to the pool and records it as in flight — on this
    // thread, at the moment of hand-off; the pool telling us back would be
    // both later and racy. The machine itself never hands off a second request
    // while one is in flight (startRequest runs from requestScaled only when
    // !running, and from onTaskFinish after running=false), so at most one
    // Scaler job is ever queued or running — no thread-count guarantee needed
    void startRequest(const ScalerRequest &req);
};
