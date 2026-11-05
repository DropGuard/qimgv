#include "thumbnailcache.h"

#include <QDateTime>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

ThumbnailCache::ThumbnailCache() {
    cacheDirPath = settings->thumbnailCacheDir();
}

QString ThumbnailCache::thumbnailPath(const QString &id) {
    return QString(cacheDirPath + id + ".png");
}

bool ThumbnailCache::exists(const QString &id) {
    QString filePath = thumbnailPath(id);
    QFileInfo file(filePath);
    return file.exists() && file.isReadable();
}

void ThumbnailCache::saveThumbnail(QImage *image, const QString &id) {
    if(image) {
        QString filePath = thumbnailPath(id);
        image->save(filePath, "PNG", 15);
    }
}

QImage *ThumbnailCache::readThumbnail(const QString &id) {
    QString filePath = thumbnailPath(id);
    QFileInfo file(filePath);
    if(file.exists() && file.isReadable()) {
        QImage *thumb = new QImage();
        if(thumb->load(filePath)) {
            // hit means "used today": keep the idle clock from expiring a
            // thumbnail that is still being displayed. utimensat because the
            // Qt setFileTime API needs the file open for write, and opening
            // a QFile for write truncates it
            const struct timespec times[2] = { { 0, UTIME_OMIT }, { 0, UTIME_NOW } };
            ::utimensat(AT_FDCWD, filePath.toLocal8Bit().constData(), times, 0);
            return thumb;
        } else {
            delete thumb;
            return nullptr;
        }
    } else {
        return nullptr;
    }
}

// returns how many entries were dropped; only files untouched for `days` can
// match, so a concurrent save (fresh mtime) is never a deletion candidate
int ThumbnailCache::purgeOlderThan(int days) {
    QDir dir(cacheDirPath);
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    int dropped = 0;
    const QStringList entries = dir.entryList(QStringList() << "*.png", QDir::Files);
    for(const QString &name : entries) {
        QFileInfo info(dir.filePath(name));
        if(info.lastModified() < cutoff && QFile::remove(dir.filePath(name)))
            dropped++;
    }
    if(dropped)
        qDebug() << "[ThumbnailCache] purged" << dropped << "of" << entries.size() << "entries idle for more than" << days << "days";
    return dropped;
}
