#pragma once

#include <QRunnable>
#include <QProcess>
#include <QThread>
#include <QCryptographicHash>
#include <ctime>
#include "sourcecontainers/thumbnail.h"
#include "components/cache/thumbnailcache.h"
#include "utils/imagefactory.h"
#include "utils/imagelib.h"
#include "settings.h"
#include <memory>
#include <QImageWriter>

class ThumbnailerRunnable : public QObject, public QRunnable {
    Q_OBJECT
public:
    ThumbnailerRunnable(ThumbnailCache* _cache, QString _path, int _size, bool _crop, bool _force);
    ~ThumbnailerRunnable() override;
    void run() override;
    static std::shared_ptr<Thumbnail> generate(ThumbnailCache *cache, QString path, int size, bool crop, bool force);
    // The frame a video decode writes out lands in a shared directory, so its
    // name must be unique per SOURCE PATH, not per basename. Public static so a
    // test can pin the seam, the way EngineBridge::extensionListFor does.
    static QString videoFrameTempPath(const QString &tmpDir, const QString &path);
private:
    static QString generateIdString(QString path, int size, bool crop);
    static std::pair<std::unique_ptr<QImage>, QSize> createThumbnail(QString path, const char* format, int size, bool crop);
    static std::pair<std::unique_ptr<QImage>, QSize> createVideoThumbnail(QString path, int size, bool crop);
    QString path;
    int size;
    bool crop, force;
    ThumbnailCache* cache = nullptr;

signals:
    void taskStart(QString, int);
    void taskEnd(std::shared_ptr<Thumbnail>, QString, bool crop);
};
