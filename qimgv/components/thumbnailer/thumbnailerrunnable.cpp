#include "thumbnailerrunnable.h"

#include "qimgv_engine.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFileInfo>
#include <utility>

ThumbnailerRunnable::ThumbnailerRunnable(ThumbnailCache* _cache, QString _path, int _size, bool _crop, bool _force,
                                         const LoadPolicy &_policy) :
    path(std::move(_path)),
    size(_size),
    crop(_crop),
    force(_force),
    policy(_policy),
    cache(_cache)
{
}

void ThumbnailerRunnable::run() {
    // Before taskStart there is nothing to settle: runningTasks, dispatched and
    // the engine's in-flight count all key off the reports, so a job that stops
    // here makes none of them and leaves no entry behind. After taskStart the
    // rules reverse — the job has announced itself, so it reports taskEnd like
    // any other, and the only thing cancellation can still save is the work.
    if(cancelRequested())
        return;
    emit taskStart(path, size);
    std::shared_ptr<Thumbnail> thumbnail = generate(cache, path, size, crop, force, policy, &cancelToken());
    emit taskEnd(thumbnail, path, crop);
}

QString ThumbnailerRunnable::videoFrameTempPath(const QString &tmpDir, const QString &path, int size, bool crop) {
    // Two clips named clip.mp4 in different folders used to be handed the same
    // temp file: one worker read (and then removed) the other's frame and cached
    // it under its own correct md5(path+size) id, which makes the wrong
    // thumbnail permanent until the cache is cleared. The basename stays in the
    // name only to keep a crashed run's leftovers recognisable. Size, crop and
    // pid are what keeps two concurrent extractions of the SAME clip from
    // sharing one file: the grid and the strip each run their own thumbnailer,
    // and so do two app instances.
    return tmpDir + QFileInfo(path).fileName() + "-" + QString::number(QCoreApplication::applicationPid())
           + "-" + generateIdString(path, size, crop) + ".png";
}

QString ThumbnailerRunnable::generateIdString(const QString& path, int size, bool crop) {
    QString queryStr = path + QString::number(size);
    if(crop)
        queryStr.append("s");
    queryStr = QString("%1").arg(QString(QCryptographicHash::hash(queryStr.toUtf8(),QCryptographicHash::Md5).toHex()));
    return queryStr;
}

std::shared_ptr<Thumbnail> ThumbnailerRunnable::generate(ThumbnailCache* cache, const QString& path, int size, bool crop, bool force,
                                                         const LoadPolicy &policy, const CancelToken *cancel) {
    // Fast path: ONE file open (header probe) for format/animation/orientation.
    // DocumentInfo (mime sniff + per-format probes, 3-5 opens) is only used for
    // videos and formats the header parser doesn't know.
    QFileInfo fileInfo(path);
    std::unique_ptr<DocumentInfo> imgInfo;
    DocumentType type;
    QString format;
    int orientation = 1;
    qimgv_probe_result pr{};
    if(qimgv_engine_probe(fileInfo.absoluteFilePath().toUtf8().constData(), &pr) == QIMGV_OK
       && pr.format != QIMGV_FMT_UNKNOWN) {
        switch(pr.format) {
            case QIMGV_FMT_JPEG: format = "jpg"; break;
            case QIMGV_FMT_PNG:  format = "png"; break;
            case QIMGV_FMT_GIF:  format = "gif"; break;
            case QIMGV_FMT_WEBP: format = "webp"; break;
            case QIMGV_FMT_AVIF: format = "avif"; break;
            case QIMGV_FMT_JXL:  format = "jxl"; break;
            case QIMGV_FMT_BMP:  format = "bmp"; break;
            case QIMGV_FMT_TIFF: format = "tiff"; break;
            default: break;
        }
        type = pr.animated ? DocumentType::ANIMATED : DocumentType::STATIC;
        orientation = (pr.orientation >= 1 && pr.orientation <= 8) ? pr.orientation : 1;
    } else {
        imgInfo = std::make_unique<DocumentInfo>(path, policy);
        type = imgInfo->type();
        format = imgInfo->format();
        orientation = imgInfo->exifOrientation();
    }

    QString thumbnailId = generateIdString(path, size, crop);
    std::unique_ptr<QImage> image;

    QString time = QString::number(fileInfo.lastModified().toMSecsSinceEpoch());

    if(!force && cache) {
        image.reset(cache->readThumbnail(thumbnailId));
        if(image && image->text("lastModified") != time)
            image.reset(nullptr);
    }

    if(!image) {
        if(type == DocumentType::NONE) {
            std::shared_ptr<Thumbnail> thumbnail(new Thumbnail(fileInfo.fileName(), "", size, nullptr));
            return thumbnail;
        }
        std::pair<std::unique_ptr<QImage>, QSize> pair;
        if(type == VIDEO)
            pair = createVideoThumbnail(path, size, crop, policy.tmpDir, policy.mpvBinary, cancel);
        else
            pair = createThumbnail(fileInfo.absoluteFilePath(), format.toStdString().c_str(), size, crop, cancel);
        image = std::move(pair.first);
        QSize originalSize = pair.second;

        image = ImageLib::exifRotated(std::move(image), orientation);

        // put in image info
        image->setText("originalWidth", QString::number(originalSize.width()));
        image->setText("originalHeight", QString::number(originalSize.height()));
        image->setText("lastModified", time);

        if(type == ANIMATED)
            image->setText("label", " [a]");
        else if(type == VIDEO)
            image->setText("label", " [v]");

        if(cache) {
            // save thumbnail if it makes sense
            // FIXME: avoid too much i/o
            if(originalSize.width() > size || originalSize.height() > size)
                cache->saveThumbnail(image.get(), thumbnailId);
        }
    }
    auto && tmpPixmap = new QPixmap(image->size());
    *tmpPixmap = QPixmap::fromImage(*image);
    tmpPixmap->setDevicePixelRatio(qGuiApp->devicePixelRatio());

    QString label;
    if(tmpPixmap->width() == 0) {
        label = "error";
    } else  {
        // put info into Thumbnail object
        label = image->text("originalWidth") +
                "x" +
                image->text("originalHeight") +
                image->text("label");
    }
    std::shared_ptr<QPixmap> pixmapPtr(tmpPixmap);
    std::shared_ptr<Thumbnail> thumbnail(new Thumbnail(fileInfo.fileName(), label, size, pixmapPtr));
    return thumbnail;
}

ThumbnailerRunnable::~ThumbnailerRunnable() {
}

std::pair<std::unique_ptr<QImage>, QSize> ThumbnailerRunnable::createThumbnail(const QString& path, const char *format, int size, bool squared,
                                                                              const CancelToken *cancel) {
    // The decode itself is one call inside QImageReader, so the checkpoint can
    // only sit in front of it. Empty means "no picture", not "no image": the
    // caller writes info into it and builds the error thumbnail from it, and a
    // null here would be dereferenced two statements later.
    if(cancel && cancel->requested())
        return std::make_pair(std::make_unique<QImage>(), QSize());
    QImageReader *reader = new QImageReader(path, format);
    Qt::AspectRatioMode ARMode = squared?
                (Qt::KeepAspectRatioByExpanding):(Qt::KeepAspectRatio);
    std::unique_ptr<QImage> result;
    QSize originalSize;
    bool indexed = (reader->imageFormat() == QImage::Format_Indexed8);
    bool manualResize = indexed || !reader->supportsOption(QImageIOHandler::Size);
    if(!manualResize) { // resize during read via QImageReader (faster)
        QSize scaledSize = reader->size().scaled(size, size, ARMode);
        reader->setScaledSize(scaledSize);
        if(squared) {
            QRect clip(0, 0, size, size);
            QRect scaledRect(QPoint(0,0), scaledSize);
            clip.moveCenter(scaledRect.center());
            reader->setScaledClipRect(clip);
        }
        originalSize = reader->size();
        result = std::make_unique<QImage>();
        if(!reader->read(result.get())) {
            // If read() returns false there's no guarantee that size conversion worked properly.
            // So we fallback to manual.
            // Se far I've seen this happen only on some weird (corrupted?) jpeg saved from camera
            manualResize = true;
            result.reset();
            // Force reset reader because it is really finicky
            // and can fail on the second read attempt (yeah wtf)
            reader->setFileName("");
            delete reader;
            reader = new QImageReader(path, format);
        }
    }
    if(manualResize) { // manual resize & crop. slower but should just work
        std::unique_ptr<QImage> fullSize = std::make_unique<QImage>();
        reader->read(fullSize.get());
        if(indexed) {
            auto newFmt = QImage::Format_RGB32;
            if(fullSize->hasAlphaChannel())
                newFmt = QImage::Format_ARGB32;
            fullSize = std::make_unique<QImage>(fullSize->convertToFormat(newFmt));
        }
        originalSize = fullSize->size();
        QSize scaledSize = fullSize->size().scaled(size, size, ARMode);
        if(squared) {
            QRect clip(0, 0, size, size);
            QRect scaledRect(QPoint(0,0), scaledSize);
            clip.moveCenter(scaledRect.center());
            QImage scaled = QImage(fullSize->scaled(scaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            result = ImageLib::croppedRaw(scaled, clip);
        } else {
            result = std::make_unique<QImage>(fullSize->scaled(scaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        }
    }
    // force reader to close file so it can be deleted later
    reader->setFileName("");
    delete reader;
    return std::make_pair(std::move(result), originalSize);
}

void ThumbnailerRunnable::waitForProcess(QProcess &process, const CancelToken *cancel, int timeoutMs) {
    if(!cancel) {
        process.waitForFinished(timeoutMs);
        return;
    }
    // 200 ms is the latency a canceled job pays before its worker is free again,
    // against the 8000 ms the wait is allowed to last: polling the flag is what
    // turns a blocked worker into a free one, and the slice only decides how fast.
    QDeadlineTimer deadline(timeoutMs);
    constexpr int slice = 200;
    while(!process.waitForFinished(slice)) {
        if(cancel->requested()) {
            // Killing is the point, and the reap after it is what makes the kill
            // final: after this returns the process is gone either way.
            process.kill();
            process.waitForFinished(slice);
            return;
        }
        // NotRunning covers both a failure to start and a finish that raced the
        // wait; either way there is nothing left to wait for.
        if(process.state() == QProcess::NotRunning || deadline.hasExpired())
            return;
    }
}

std::pair<std::unique_ptr<QImage>, QSize> ThumbnailerRunnable::createVideoThumbnail(const QString& path, int size, bool squared,
                                                                                    const QString &tmpDir, const QString &mpvBinary,
                                                                                    const CancelToken *cancel) {
    QImageReader reader;
    const QString tmpFilePath = videoFrameTempPath(tmpDir, path, size, squared);
    QString tmpFilePathEsc = tmpFilePath;
    tmpFilePathEsc.replace("%", "%%");
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(mpvBinary,
                  QStringList() << "--start=30%"
                                << "--frames=1"
                                << "--aid=no"
                                << "--sid=no"
                                << "--no-config"
                                << "--load-scripts=no"
                                << "--no-terminal"
                                << "--o=" + tmpFilePathEsc
                                << path
                  );
    waitForProcess(process, cancel, 8000);
    process.close();

    reader.setFileName(tmpFilePath);
    reader.setFormat("png");
    Qt::AspectRatioMode ARMode = squared?
                (Qt::KeepAspectRatioByExpanding):(Qt::KeepAspectRatio);
    std::unique_ptr<QImage> result;

    // scale & crop
    QSize scaledSize = reader.size().scaled(size, size, ARMode);
    reader.setScaledSize(scaledSize);
    if(squared) {
        QRect clip(0, 0, size, size);
        QRect scaledRect(QPoint(0,0), scaledSize);
        clip.moveCenter(scaledRect.center());
        reader.setScaledClipRect(clip);
    }
    QSize originalSize = reader.size();
    result = std::make_unique<QImage>(reader.read());

    // force reader to close file so it can be deleted later
    reader.setFileName("");

    // remove temporary file
    QFile tmpFile(tmpFilePath);
    tmpFile.remove();

    return std::make_pair(std::move(result), originalSize);
}
