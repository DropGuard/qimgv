#pragma once

#include <QProcess>
#include <QThread>
#include <QCryptographicHash>
#include <ctime>
#include "sourcecontainers/thumbnail.h"
#include "components/cache/thumbnailcache.h"
#include "components/workerpool/workerpool.h"
#include "utils/imagefactory.h"
#include "utils/imagelib.h"
#include "settings.h"
#include <memory>
#include <QImageWriter>

class ThumbnailerRunnable : public QObject, public PoolJob {
    Q_OBJECT
public:
    ThumbnailerRunnable(ThumbnailCache* _cache, QString _path, int _size, bool _crop, bool _force,
                        const LoadPolicy &_policy);
    ~ThumbnailerRunnable() override;
    void run() override;
    static std::shared_ptr<Thumbnail> generate(ThumbnailCache *cache, const QString& path, int size, bool crop, bool force,
                                               const LoadPolicy &policy, const CancelToken *cancel = nullptr);
    // The frame a video decode writes out lands in a shared directory, so its
    // name must be unique per SOURCE PATH, not per basename. Public static so a
    // test can pin the seam, the way EngineBridge::extensionListFor does.
    static QString videoFrameTempPath(const QString &tmpDir, const QString &path);
    // Waits for the frame extractor the way a cancellable job has to: in slices,
    // so a job that is no longer wanted can stop a wait that would otherwise hold
    // a worker for the whole timeout. A null cancel waits in one call, which is
    // what the synchronous Thumbnailer::getThumbnail does. Public static for the
    // same reason as above — a test pins it against a real process.
    static void waitForProcess(QProcess &process, const CancelToken *cancel, int timeoutMs);
private:
    static QString generateIdString(const QString& path, int size, bool crop);
    static std::pair<std::unique_ptr<QImage>, QSize> createThumbnail(const QString& path, const char* format, int size, bool crop,
                                                                     const CancelToken *cancel);
    static std::pair<std::unique_ptr<QImage>, QSize> createVideoThumbnail(const QString& path, int size, bool crop,
                                                                          const QString &tmpDir, const QString &mpvBinary,
                                                                          const CancelToken *cancel);
    QString path;
    int size;
    bool crop, force;
    // the environment, carried from the dispatch site: the pool thread never
    // reads the settings global
    LoadPolicy policy;
    ThumbnailCache* cache = nullptr;

signals:
    void taskStart(QString, int);
    void taskEnd(std::shared_ptr<Thumbnail>, QString, bool crop);
};
