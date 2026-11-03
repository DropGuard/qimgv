#include "imagefactory.h"

std::optional<std::shared_ptr<Image>> ImageFactory::createImage(QString path, bool scaled) {
    std::unique_ptr<DocumentInfo> docInfo(new DocumentInfo(path));
    auto type = docInfo->type();
    if(type == NONE) {
        qDebug() << "ImageFactory: cannot load " << docInfo->filePath();
        return std::nullopt;
    }
    if(type == ANIMATED)
        return std::make_shared<ImageAnimated>(std::move(docInfo));
    if(type == VIDEO)
        return std::make_shared<Video>(std::move(docInfo));
    // forward the tier request: dropping it here silently disabled the
    // whole two-tier pipeline (every load decoded full and reported
    // [full]; ImageStatic::loadGeneric never scaled anything)
    return std::make_shared<ImageStatic>(std::move(docInfo), scaled);
}
