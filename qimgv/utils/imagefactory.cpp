#include "imagefactory.h"

std::shared_ptr<Image> ImageFactory::createImage(QString path, bool scaled) {
    std::unique_ptr<DocumentInfo> docInfo(new DocumentInfo(path));
    std::shared_ptr<Image> img = nullptr;
    if(docInfo->type() == NONE) {
        qDebug() << "ImageFactory: cannot load " << docInfo->filePath();
    } else if(docInfo->type() == ANIMATED) {
        img.reset(new ImageAnimated(std::move(docInfo)));
    } else if(docInfo->type() == VIDEO) {
        img.reset(new Video(std::move(docInfo)));
    } else {
        // forward the tier request: dropping it here silently disabled the
        // whole two-tier pipeline (every load decoded full and reported
        // [full]; ImageStatic::loadGeneric never scaled anything)
        img.reset(new ImageStatic(std::move(docInfo), scaled));
    }
    return img;
}
