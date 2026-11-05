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
    explicit ThumbnailCache();

    // entries expire by IDLE time, not age: readThumbnail refreshes mtime on a
    // hit, so anything still being displayed survives while abandoned size
    // variants (and orphans of renamed/deleted sources) age out
    static constexpr int cacheMaxIdleDays = 15;

    void saveThumbnail(QImage *image, const QString &id);
    QImage* readThumbnail(const QString &id);
    QString thumbnailPath(const QString &id);
    bool exists(const QString &id);
    int purgeOlderThan(int days);

signals:

public slots:

private:
    // we are still bottlenecked by disk access anyway
    QMutex mutex;
    QString cacheDirPath;
};
