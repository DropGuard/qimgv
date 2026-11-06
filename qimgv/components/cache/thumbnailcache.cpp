#include "thumbnailcache.h"

#include <QDateTime>
#include <QVector>

#include <algorithm>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

ThumbnailCache::ThumbnailCache(const QString &_cacheDirPath)
    : cacheDirPath(_cacheDirPath) {
}

QString ThumbnailCache::thumbnailPath(const QString &id) {
    // not cacheDirPath + id: concatenating assumed the directory came with a
    // trailing separator, which only settings->thumbnailCacheDir() ever
    // provided. QDir::filePath makes the caller's spelling irrelevant
    return QDir(cacheDirPath).filePath(id + QStringLiteral(".png"));
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

// One walk, two passes: the stat that decides idle expiry is the stat that
// measures the cache, so the byte fuse costs nothing the sweep was not already
// paying. Returns what each pass did, because "the cap fired" is a different
// fact from "the library is old" and a log line has to tell them apart.
ThumbnailCache::PurgeReport ThumbnailCache::purgeOlderThan(int days, qint64 byteCap) {
    QDir dir(cacheDirPath);
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    PurgeReport report;

    struct Entry { QString path; qint64 size; QDateTime mtime; };
    QVector<Entry> kept;
    const QStringList entries = dir.entryList(QStringList() << "*.png", QDir::Files);
    kept.reserve(entries.size());
    qint64 bytes = 0;
    for(const QString &name : entries) {
        const QString path = dir.filePath(name);
        const QFileInfo info(path);
        // only files untouched for `days` can match, so a concurrent save
        // (fresh mtime) is never an expiry candidate
        if(info.lastModified() < cutoff) {
            if(QFile::remove(path)) {
                report.idleDropped++;
                continue;
            }
            // removal failed (read-only, busy): it still occupies space, so
            // it stays in the census rather than vanishing from the total
        }
        bytes += info.size();
        kept.append({path, info.size(), info.lastModified()});
    }

    // the fuse: reached only when the idle sweep left the cache over the cap.
    // Oldest mtime first, which is least-recently-used, so the entries that
    // survive are the ones being displayed. What this can reach that the idle
    // pass could not is an entry written seconds ago; losing one of those is a
    // cache miss and a regeneration, not a wrong image — the write itself is
    // unbothered, since unlinking an open file only removes the name.
    report.bytesAfter = bytes;
    if(bytes > byteCap) {
        std::sort(kept.begin(), kept.end(), [](const Entry &a, const Entry &b) {
            return a.mtime < b.mtime;
        });
        for(const Entry &e : kept) {
            if(report.bytesAfter <= byteCap)
                break;
            if(QFile::remove(e.path)) {
                report.bytesAfter -= e.size;
                report.sizeEvicted++;
            }
        }
        qDebug() << "[ThumbnailCache] byte cap" << byteCap << "fired:"
                 << report.sizeEvicted << "entries evicted least-recently-used first,"
                 << report.bytesAfter << "bytes remain";
    }
    if(report.idleDropped)
        qDebug() << "[ThumbnailCache] purged" << report.idleDropped << "of" << entries.size()
                 << "entries idle for more than" << days << "days";
    return report;
}
