#include "loaderrunnable.h"

#include <QLoggingCategory>
Q_LOGGING_CATEGORY(lcLoaderPerf, "qimgv.perf.loader", QtInfoMsg)

#include <QElapsedTimer>

LoaderRunnable::LoaderRunnable(QString _path, bool _scaled, const LoadPolicy &_policy) :
    path(_path),
    scaled(_scaled),
    policy(_policy) {
}

void LoaderRunnable::run() {
    // The one checkpoint a decode has: after this call the bytes are already
    // decoded and the work is spent. A job marked before it gets here drops the
    // decode and reports nothing, which is the loader's rule rather than a pool
    // rule — an unstarted job has no entry in the engine's accounting to settle,
    // and reporting a *failure* for a file the user already left would turn "no
    // longer wanted" into "this file is broken" on screen. Loader::clearTasks()
    // drops the task entry for a job that reports nothing.
    if(cancelRequested())
        return;
    QElapsedTimer t;
    t.start();
    auto image = ImageFactory::createImage(path, scaled, policy);
    // the RESULT decides the tier: only formats with native scaled decode
    // (JPEG) produce a tier-1 image; everything else decodes full here
    bool resultScaled = scaled && image.has_value() && (*image)->isScaledResult();
    qCDebug(lcLoaderPerf) << "L:" << t.elapsed() << "ms" << (resultScaled ? "[scaled]" : "[full]");
    emit finished(image, path, resultScaled);
}
