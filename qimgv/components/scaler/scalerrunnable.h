#pragma once

#include <QObject>
#include "components/workerpool/workerpool.h"
#include "scalerrequest.h"

// one-shot: a fresh runnable per request, owned by the pool from submit() to
// the moment run() returns
class ScalerRunnable : public QObject, public PoolJob
{
    Q_OBJECT
public:
    explicit ScalerRunnable(ScalerRequest r);
    void run() override;
signals:
    // the result is shared, not handed over as a naked pointer: this signal is
    // emitted on a pool thread
    void finished(std::shared_ptr<const QImage>, ScalerRequest);

private:
    ScalerRequest req;
};
