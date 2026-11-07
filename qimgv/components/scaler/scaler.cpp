#include "scaler.h"

/* What this should do in theory:
 * 1 request comes
 * 2 we run it
 * 3a if during scaling no new requests came, we return the result and forget about it. end.
 * 3b if some requests did come, by the end of current task we dispose of its result,
 *    start the last task that came and ignore the middle ones.
 */

Scaler::Scaler(Cache *_cache, WorkerPool &_pool, QObject *parent)
    : QObject(parent),
      pool(_pool),
      buffered(false),
      running(false),
      cache(_cache)
{
    // no explicit connection type: `finished` is emitted on a pool thread and
    // received here, so it is queued by construction and the state machine runs
    // on the UI thread. DirectConnection used to run these slots on the pool
    // thread, where they wrote `running`/`buffered`/`startedRequest` and reached
    // Cache's QMap through a semaphore that nothing else in the app shared.
    connect(this, &Scaler::acceptScalingResult, this, &Scaler::slotForwardScaledResult, Qt::QueuedConnection);
}

Scaler::~Scaler() {
    // at most one job is out (queued or running): the machine never hands off a
    // second while one is in flight. Whatever is queued comes back from drain()
    // and dies with the vector; a running one the pool destroys on its worker
    // thread when run() returns. Reservations still held at this point are not
    // released — moot, since the Cache that owns them dies right after.
    auto dropped = pool.drain(PoolOwner::Scaler);
    pool.waitForOwner(PoolOwner::Scaler);
}

void Scaler::requestScaled(const ScalerRequest &req) {
    Q_ASSERT(QThread::currentThread() == thread());
    if(!running) {
//////////////////////////////////
        if(!buffered) {
            bufferedRequest = req;
            buffered = true;
          //qDebug() << "1 requestScaled() - locking..  " <<  req.image->name();
            cache->reserve(req.image->filePath());
          //qDebug() << "1 requestScaled() - LOCKED!  " <<  req.image->name();
            startRequest(req);
        } else if(bufferedRequest.image != req.image) {
          //qDebug() << "2 requestScaled() - locking...  " <<  req.image->name();
            cache->reserve(req.image->filePath());
          //qDebug() << "2 requestScaled() - LOCKED!  " <<  req.image->name();
            auto tmp = bufferedRequest;
            bufferedRequest = req;
            buffered = true;
            if(startedRequest.image != tmp.image) {
                cache->release(tmp.image->filePath());
              //qDebug() << "2 requestScaled() - RELEASED!  " <<  tmp.image->name();
            }
        } else {
            bufferedRequest = req;
            buffered = true;
        }
//////////////////////////////
    } else {
        if(!buffered) {
            if(req.image != startedRequest.image)
                cache->reserve(req.image->filePath());
            bufferedRequest = req;
            buffered = true;
        } else {
            if(req.image == bufferedRequest.image) {
                bufferedRequest = req;
                buffered = true;
            } else {
                if(bufferedRequest.image != startedRequest.image) {
                    //qDebug() << "4 RELEASING " << bufferedRequest.image->name();
                    cache->release(bufferedRequest.image->filePath());
                }
                if(req.image != startedRequest.image)
                    cache->reserve(req.image->filePath());
                bufferedRequest = req;
                buffered = true;
            }
        }
    }
}

void Scaler::onTaskFinish(std::shared_ptr<const QImage> scaled, const ScalerRequest &req) {
    Q_ASSERT(QThread::currentThread() == thread());
    running = false;
    if(buffered && bufferedRequest.image == req.image) {
    } else {
      //qDebug() << "onTaskFinish() - 2 releasing..  " <<  req.image->name();
        cache->release(req.image->filePath());
      //qDebug() << "onTaskFinish() - 2 RELEASED!  " <<  name;
    }
    if(buffered) {
      //qDebug() << "onTaskFinish - startingBuffered: " << bufferedRequest.string;
        // the discarded result frees itself
        startRequest(bufferedRequest);
    } else {
        emit acceptScalingResult(scaled, req);
    }
}

void Scaler::slotForwardScaledResult(std::shared_ptr<const QImage> image, const ScalerRequest &req) {
    if(!image)
        return;
    // QPixmap is implicitly shared, so the consumer can take it by value and the
    // question of who deletes it disappears with the pointer
    emit scalingFinished(QPixmap::fromImage(*image), req);
}

void Scaler::startRequest(const ScalerRequest &req) {
    running = true;
    startedRequest = req;
    // clear the buffered flag if there were no requests after this one
    if(buffered && bufferedRequest == req)
        buffered = false;
    auto job = std::make_unique<ScalerRunnable>(req);
    connect(job.get(), &ScalerRunnable::finished, this, &Scaler::onTaskFinish);
    pool.submit(PoolOwner::Scaler, 1, std::move(job));
}
