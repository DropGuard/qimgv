#include "loaderrunnable.h"

#include <QElapsedTimer>

LoaderRunnable::LoaderRunnable(QString _path, bool _scaled) : path(_path), scaled(_scaled) {
}

void LoaderRunnable::run() {
    QElapsedTimer t;
    t.start();
    auto image = ImageFactory::createImage(path, scaled);
    qDebug() << "L:" << t.elapsed() << "ms" << (scaled ? "[scaled]" : "[full]") << path;
    emit finished(image, path, scaled);
}
