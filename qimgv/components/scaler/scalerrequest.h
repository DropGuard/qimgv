#ifndef SCALERREQUEST_H
#define SCALERREQUEST_H

#include <QPixmap>
#include <utility>
#include "sourcecontainers/image.h"
#include "settings.h" // move enums somewhere else?

class ScalerRequest {
public:
    ScalerRequest() : image(nullptr), size(QSize(0,0)), filter(QI_FILTER_BILINEAR) { }
    ScalerRequest(std::shared_ptr<Image> _image, QSize _size, QString _path, ScalingFilter _filter,
                  bool _smoothUpscaling) : image(std::move(_image)), size(_size), path(std::move(_path)), filter(_filter),
                  smoothUpscaling(_smoothUpscaling) {}
    std::shared_ptr<Image> image;
    QSize size;
    QString path;
    ScalingFilter filter;
    // Read on the UI thread and carried along: the pool worker used to call
    // settings->smoothUpscaling() itself, and sharing one QSettings instance
    // across threads is a data race (Qt documents it as needing external
    // synchronization). A value in the request also pins the setting to the
    // moment the request was made.
    bool smoothUpscaling = true;

    // Equality means "the same scaled result", because Scaler::startRequest
    // uses it as the guard before clearing its buffer: a buffered request that
    // compares equal to the one just started is treated as already done. So
    // every field the worker reads has to be covered - smoothUpscaling decides
    // NEAREST-versus-filter scaling, and leaving it out made two differently-
    // scaling requests interchangeable. `path` is deliberately not part of it:
    // it identifies the file the result is for (Core checks that on delivery),
    // not what the result is.
    bool operator==(const ScalerRequest &another) const {
        return another.image == image && another.size == size
            && another.filter == filter && another.smoothUpscaling == smoothUpscaling;
    }
};

#endif // SCALERREQUEST_H
