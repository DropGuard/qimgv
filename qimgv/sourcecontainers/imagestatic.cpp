#include "imagestatic.h"

#include "utils/fileoperations.h"

#include <QScreen>
#include <time.h>

ImageStatic::ImageStatic(std::unique_ptr<DocumentInfo> _info, bool scaled, const LoadPolicy &policy)
    : Image(std::move(_info)),
      mScaledRequest(scaled),
      mAllocationLimitMb(policy.allocationLimitMb)
{
    load();
}

ImageStatic::~ImageStatic() {
}

//load image data from disk
void ImageStatic::load() {
    if(mLoadAttempted) {
        return;
    }
    if(mDocInfo->mimeType().name() == "image/vnd.microsoft.icon")
        loadICO();
    else
        loadGeneric();
}


void ImageStatic::loadGeneric() {
    // two-tier decode: scaled target = primary screen at devicePixelRatio
    /* QImageReader::read() seems more reliable than just reading via QImage.
     * For example: "Invalid JPEG file structure: two SOF markers"
     * QImageReader::read() returns false, but still reads an image. Meanwhile QImage just fails.
     * I havent checked qimage's code, but it seems like it sees an exception
     * from libjpeg or whatever and just gives up on reading the file.
     *
     * tldr: qimage bad
     */
    QImageReader r(mPath, mDocInfo->format().toStdString().c_str());
    r.setAllocationLimit(mAllocationLimitMb);
    if(mScaledRequest && mDocInfo->format() == "jpg") {
        // tier-1: decode at screen resolution via libjpeg DCT scaling
        // (1/2 edge = 1/4 pixels). Full-res follows as tier-2 upgrade.
        // JPEG-only: other formats scale AFTER full decode in Qt, which
        // costs more than it saves (measured: PNG scaled 84ms vs full 46ms).
        QSize orig = r.size();
        QSize screen = QApplication::primaryScreen()->size()
                       * QApplication::primaryScreen()->devicePixelRatio();
        if(orig.isValid() && (orig.width() > screen.width() || orig.height() > screen.height())) {
            r.setScaledSize(orig.scaled(screen, Qt::KeepAspectRatio));
            mScaledApplied = true;
        }
    }
    QImage *tmp = new QImage();
    r.read(tmp);
    std::unique_ptr<const QImage> img(tmp);
    img = ImageLib::exifRotated(std::move(img), mDocInfo.get()->exifOrientation());
    // scaling this format via qt results in transparent background
    // it rare enough so lets just convert it to the closest working thing
    if(img->format() == QImage::Format_Mono) {
        QImage *imgConverted = new QImage();
        *imgConverted = img->convertToFormat(QImage::Format_Grayscale8);
        image.reset(imgConverted);
    } else {
        // set image
        image = std::move(img);
    }
    mLoadAttempted = true;
}

// TODO: move this out somewhere to use in other places
void ImageStatic::loadICO() {
    // Big brain code. It's mostly for small ico files so whatever. I'm not patching Qt for this.
    QIcon icon(mPath);
    QList<QSize> sizes = icon.availableSizes();
    QSize maxSize(0, 0);
    for(auto sz : sizes)
        if(maxSize.width() < sz.width())
            maxSize = sz;
    QPixmap iconPix = icon.pixmap(maxSize);
    std::unique_ptr<const QImage> img(new QImage(iconPix.toImage()));
    image = std::move(img);
    mLoadAttempted = true;
}

bool ImageStatic::save(QString destPath, int jpegQuality) {
    // The backup, the extension/quality decision and the rollback live in
    // FileOperations::saveWithBackup now, shared with the clipboard import:
    // two copies of that sequence had already diverged once.
    const bool wasEdited = isEdited();
    const bool success = FileOperations::saveWithBackup(*(wasEdited ? imageEdited : image),
                                                        destPath, jpegQuality);
    // An edit is consumed by a write that actually happened. Swapping and
    // discarding before consulting `success` threw the user's unsaved edit away
    // on every failed save, and left the in-memory image claiming it was saved.
    if(success && wasEdited) {
        image.swap(imageEdited);
        discardEditedImage();
    }
    if(destPath == mPath && success)
        mDocInfo->refresh();
    return success;
}

std::unique_ptr<QPixmap> ImageStatic::getPixmap() {
    std::unique_ptr<QPixmap> pix(new QPixmap());
    isEdited()?pix->convertFromImage(*imageEdited):pix->convertFromImage(*image, Qt::NoFormatConversion);
    return pix;
}

std::shared_ptr<const QImage> ImageStatic::getSourceImage() {
    return image;
}

std::shared_ptr<const QImage> ImageStatic::getImage() {
    return isEdited()?imageEdited:image;
}

int ImageStatic::height() {
    return isEdited()?imageEdited->height():image->height();
}

int ImageStatic::width() {
    return isEdited()?imageEdited->width():image->width();
}

QSize ImageStatic::size() {
    return isEdited()?imageEdited->size():image->size();
}

bool ImageStatic::setEditedImage(std::unique_ptr<const QImage> imageEditedNew) {
    if(imageEditedNew && imageEditedNew->width() != 0) {
        discardEditedImage();
        imageEdited = std::move(imageEditedNew);
        mEdited = true;
        return true;
    }
    return false;
}

bool ImageStatic::discardEditedImage() {
    if(imageEdited) {
        imageEdited.reset();
        mEdited = false;
        return true;
    }
    return false;
}
