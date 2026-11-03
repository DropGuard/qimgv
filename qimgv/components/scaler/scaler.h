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
    void scalingFinished(QPixmap result, const ScalerRequest &request);
    void acceptScalingResult(std::shared_ptr<const QImage> image, const ScalerRequest &req);
    void startBufferedRequest();

public slots:
    void requestScaled(const ScalerRequest &req);

private slots:
    void onTaskStart(const ScalerRequest &req);
    void onTaskFinish(std::shared_ptr<const QImage> scaled, const ScalerRequest &req);
    void slotStartBufferedRequest();
    void slotForwardScaledResult(std::shared_ptr<const QImage> image, const ScalerRequest &req);

private:
    QThreadPool *pool = nullptr;
    ScalerRunnable *runnable;
    bool buffered, running;
    clock_t currentRequestTimestamp;
    ScalerRequest bufferedRequest, startedRequest;

    Cache *cache;

    void startRequest(const ScalerRequest &req);

    QSemaphore *sem = nullptr;
};
