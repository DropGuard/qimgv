#pragma once

#include <QObject>
#include <QThreadPool>
#include <QThread>
#include <QMutex>
#include "components/cache/cache.h"
#include "scalerrequest.h"
#include "scalerrunnable.h"

class Scaler : public QObject {
    Q_OBJECT
public:
    explicit Scaler(Cache *_cache, QObject *parent = nullptr);

signals:
    void scalingFinished(QPixmap result, ScalerRequest request);
    void acceptScalingResult(std::shared_ptr<const QImage> image, ScalerRequest req);
    void startBufferedRequest();

public slots:
    void requestScaled(ScalerRequest req);

private slots:
    void onTaskStart(ScalerRequest req);
    void onTaskFinish(std::shared_ptr<const QImage> scaled, ScalerRequest req);
    void slotStartBufferedRequest();
    void slotForwardScaledResult(std::shared_ptr<const QImage> image, ScalerRequest req);

private:
    QThreadPool *pool = nullptr;
    ScalerRunnable *runnable;
    bool buffered, running;
    clock_t currentRequestTimestamp;
    ScalerRequest bufferedRequest, startedRequest;

    Cache *cache;

    void startRequest(ScalerRequest req);

    QSemaphore *sem = nullptr;
};
