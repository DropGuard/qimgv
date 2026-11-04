#include "imagestatic.h"

#include <QScreen>
#include <time.h>

ImageStatic::ImageStatic(QString _path)
    : Image(_path)
{
    load();
}

ImageStatic::ImageStatic(std::unique_ptr<DocumentInfo> _info, bool scaled)
    : Image(std::move(_info)), mScaledRequest(scaled)
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
    r.setAllocationLimit(settings->memoryAllocationLimit());
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

QString ImageStatic::generateHash(QString str) {
    return QString(QCryptographicHash::hash(str.toUtf8(), QCryptographicHash::Md5).toHex());
}

// TODO: move saving to directorymodel
bool ImageStatic::save(QString destPath) {
    QString tmpPath = destPath + "_" + generateHash(destPath);
    QFileInfo fi(destPath);
    QString ext = fi.suffix();
    // png compression note from libpng
    // Note that tests have shown that zlib compression levels 3-6 usually perform as well
    // as level 9 for PNG images, and do considerably fewer caclulations
    int quality = 95;
    if(ext.compare("png", Qt::CaseInsensitive) == 0)
        quality = 30;
    else if(ext.compare("jpg", Qt::CaseInsensitive) == 0 || ext.compare("jpeg", Qt::CaseInsensitive) == 0)
        quality = settings->JPEGSaveQuality();

    bool backupExists = false, success = false, originalExists = false;

    if(QFile::exists(destPath))
        originalExists = true;

    // backup the original file if possible
    if(originalExists) {
        QFile::remove(tmpPath);
        if(!QFile::copy(destPath, tmpPath)) {
            qDebug() << "ImageStatic::save() - Could not create file backup.";
            return false;
        }
        backupExists = true;
    }
    // save file
    const bool wasEdited = isEdited();
    if(wasEdited)
        success = imageEdited->save(destPath, ext.toStdString().c_str(), quality);
    else
        success = image->save(destPath, ext.toStdString().c_str(), quality);
    // An edit is consumed by a write that actually happened. Swapping and
    // discarding before consulting `success` threw the user's unsaved edit away
    // on every failed save, and left the in-memory image claiming it was saved.
    if(success && wasEdited) {
        image.swap(imageEdited);
        discardEditedImage();
    }
    if(backupExists) {
        if(success) {
            QFile::remove(tmpPath);
        } else {
            // Restore the backup to the path it was taken FROM, which is
            // destPath. This used to delete mDocInfo->filePath() and copy the
            // backup there — a different file whenever this was a Save As, so the
            // rollback destroyed the original while undoing a write to something
            // else. And if the restore itself failed, the backup was deleted
            // anyway, which turns "save failed" into "your file is gone".
            bool restored = false;
            if(!QFile::exists(destPath) || QFile::remove(destPath))
                restored = QFile::copy(tmpPath, destPath);
            if(restored)
                QFile::remove(tmpPath);
            else
                qCritical() << "[ImageStatic] save failed AND the backup could not be"
                            << "restored; the previous contents of" << destPath
                            << "are still in" << tmpPath;
        }
    }
    if(destPath == mPath && success)
        mDocInfo->refresh();
    return success;
}

bool ImageStatic::save() {
    return save(mPath);
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
