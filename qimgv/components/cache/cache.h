#pragma once

#include <QDebug>
#include <QMap>
#include <QSemaphore>
#include <QMutexLocker>
#include "sourcecontainers/image.h"
#include "components/cache/cacheitem.h"
#include "utils/imagefactory.h"

class Cache {
public:
    explicit Cache();
    ~Cache();
    bool contains(const QString &path) const;
    void remove(const QString &path);
    void clear();

    bool insert(std::shared_ptr<Image> img);
    void trimTo(const QStringList &list);

    std::shared_ptr<Image> get(const QString &path);
    bool release(const QString &path);
    bool reserve(const QString &path);
    const QList<QString> keys() const;

private:
    QMap<QString, CacheItem*> items;
};
