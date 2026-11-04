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

    bool operator==(const ScalerRequest &another) const {
        if(another.image == image && another.size == size && another.filter == filter)
            return true;
        return false;
    }
};

#endif // SCALERREQUEST_H
