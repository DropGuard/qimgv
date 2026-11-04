#include "imagefactory.h"

#include <utility>

std::optional<std::shared_ptr<Image>> ImageFactory::createImage(QString path, bool scaled, const LoadPolicy &policy) {
    std::unique_ptr<DocumentInfo> docInfo(new DocumentInfo(std::move(path), policy));
    auto type = docInfo->type();
    if(type == NONE) {
        qDebug() << "ImageFactory: cannot load " << docInfo->filePath();
        return std::nullopt;
    }
    if(type == ANIMATED) {
        auto image = std::make_shared<ImageAnimated>(std::move(docInfo));
        if(!image->getImage() || image->getImage()->isNull())
            return std::nullopt;
        return image;
    }
    if(type == VIDEO)
        // frames arrive from the player at playback; a null here is by design
        return std::make_shared<Video>(std::move(docInfo));
    // forward the tier request: dropping it here silently disabled the
    // whole two-tier pipeline (every load decoded full and reported
    // [full]; ImageStatic::loadGeneric never scaled anything)
    const QString imagePath = docInfo->filePath();
    auto image = std::make_shared<ImageStatic>(std::move(docInfo), scaled, policy);
    // an undecodable file used to arrive as a "loaded" object holding a null
    // image — the view showed nothing, the cache cached it, and only a
    // null-check on the pixels could tell. Absence is the case the loader
    // already speaks: the decode simply failed.
    if(!image->getImage() || image->getImage()->isNull()) {
        qDebug() << "ImageFactory: decode produced no pixels for" << imagePath;
        return std::nullopt;
    }
    return image;
}
