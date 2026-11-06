#pragma once

#include <QObject>
#include <QDir>
#include <QMutex>
#include <QDebug>
#include "settings.h"
#include "sourcecontainers/thumbnail.h"

class ThumbnailCache : public QObject
{
    Q_OBJECT
public:
    // the directory is a parameter, not a settings lookup in the constructor:
    // a purge deletes files, so a test that measures it must own the directory
    // it points at rather than share the app's with every other suite
    explicit ThumbnailCache(const QString &_cacheDirPath);

    // entries expire by IDLE time, not age: readThumbnail refreshes mtime on a
    // hit, so anything still being displayed survives while abandoned size
    // variants (and orphans of renamed/deleted sources) age out
    static constexpr int cacheMaxIdleDays = 15;
    // The byte cap is a fuse, not a policy: it is consulted only after the idle
    // sweep has already run, so a library that lives inside its idle window
    // never loses an entry to it. mtime is the eviction order because, as above,
    // mtime is the last-used clock — which makes the fuse an LRU by reuse of the
    // existing mechanism rather than a second bookkeeping system.
    static constexpr qint64 cacheMaxBytes = 500LL * 1024 * 1024;

    struct PurgeReport {
        int idleDropped = 0;   // untouched for `days` or more
        int sizeEvicted = 0;   // sacrificed to bring the cache under the cap
        qint64 bytesAfter = 0; // what the cache holds when both passes are done
    };

    void saveThumbnail(QImage *image, const QString &id);
    QImage* readThumbnail(const QString &id);
    QString thumbnailPath(const QString &id);
    bool exists(const QString &id);
    PurgeReport purgeOlderThan(int days, qint64 byteCap);

signals:

public slots:

private:
    // we are still bottlenecked by disk access anyway
    QMutex mutex;
    QString cacheDirPath;
};
