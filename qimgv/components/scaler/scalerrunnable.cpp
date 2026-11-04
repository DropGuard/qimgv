#include "scalerrunnable.h"

#include <QElapsedTimer>
#include <utility>

ScalerRunnable::ScalerRunnable() {
}

void ScalerRunnable::setRequest(ScalerRequest r) {
    req = std::move(r);
}

void ScalerRunnable::run() {
    //QElapsedTimer t;
    //t.start();
    std::shared_ptr<const QImage> scaled;
    if(req.filter == 0 || (req.size.width() > req.image->width() && !req.smoothUpscaling)) {
        scaled = std::shared_ptr<const QImage>(ImageLib::scaled(req.image->getImage(), req.size, QI_FILTER_NEAREST));
    } else {
        scaled = std::shared_ptr<const QImage>(ImageLib::scaled(req.image->getImage(), req.size, req.filter));
    }
    //qDebug() << ">> " << req.size << ": " << t.elapsed();
    emit finished(scaled, req);
}
