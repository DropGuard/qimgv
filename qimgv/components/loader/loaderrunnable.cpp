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
    QElapsedTimer t;
    t.start();
    auto image = ImageFactory::createImage(path, scaled, policy);
    // the RESULT decides the tier: only formats with native scaled decode
    // (JPEG) produce a tier-1 image; everything else decodes full here
    bool resultScaled = scaled && image.has_value() && (*image)->isScaledResult();
    qCDebug(lcLoaderPerf) << "L:" << t.elapsed() << "ms" << (resultScaled ? "[scaled]" : "[full]");
    emit finished(image, path, resultScaled);
}
