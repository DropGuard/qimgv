#include "loaderrunnable.h"

#include <QElapsedTimer>

LoaderRunnable::LoaderRunnable(QString _path, bool _scaled) : path(_path), scaled(_scaled) {
}

void LoaderRunnable::run() {
    QElapsedTimer t;
    t.start();
    auto image = ImageFactory::createImage(path, scaled);
    // the RESULT decides the tier: only formats with native scaled decode
    // (JPEG) produce a tier-1 image; everything else decodes full here
    bool resultScaled = scaled && image && image->isScaledResult();
    qDebug() << "L:" << t.elapsed() << "ms" << (resultScaled ? "[scaled]" : "[full]");
    emit finished(image, path, resultScaled);
}
