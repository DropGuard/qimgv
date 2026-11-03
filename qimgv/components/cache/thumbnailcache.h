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

    void saveThumbnail(QImage *image, const QString &id);
    QImage* readThumbnail(const QString &id);
    QString thumbnailPath(const QString &id);
    bool exists(const QString &id);

signals:

public slots:

private:
    // we are still bottlenecked by disk access anyway
    QMutex mutex;
    QString cacheDirPath;
};
