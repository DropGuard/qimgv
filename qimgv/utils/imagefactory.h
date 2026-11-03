#pragma once

#include <optional>

#include "utils/imagelib.h"
#include "sourcecontainers/documentinfo.h"
#include "sourcecontainers/image.h"
#include "sourcecontainers/imageanimated.h"
#include "sourcecontainers/imagestatic.h"
#include "sourcecontainers/video.h"

class ImageFactory {
public:
    // nullopt when the file is not a document type this app can open. Not a
    // null shared_ptr: the hops between here and the loader all have to name
    // the case they are in, which is the one thing a nullable pointer cannot
    // ask of a compiler.
    static std::optional<std::shared_ptr<Image>> createImage(QString path, bool scaled = false);
};
