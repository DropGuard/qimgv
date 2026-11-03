#include "cache.h"

Cache::Cache() {
}

bool Cache::contains(QString path) const {
    return items.contains(path);
}

bool Cache::insert(std::shared_ptr<Image> img) {
    if(!img)
        return false; // reporting success for a failed decode hides the failure
    if(items.contains(img->filePath()))
        return false;
    items.insert(img->filePath(), new CacheItem(img));
    return true;
}

void Cache::remove(QString path) {
    if(items.contains(path)) {
        items[path]->lock();
        auto *item = items.take(path);
        delete item;
    }
}

void Cache::clear() {
    for(auto it = items.begin(); it != items.end(); ++it) {
        it.value()->lock();
        delete it.value();
    }
    items.clear();
}

std::shared_ptr<Image> Cache::get(QString path) {
    if(items.contains(path)) {
        CacheItem *item = items.value(path);
        return item->getContents();
    }
    return nullptr;
}

bool Cache::reserve(QString path) {
    if(items.contains(path)) {
        items[path]->lock();
        return true;
    }
    return false;
}

bool Cache::release(QString path) {
    if(items.contains(path)) {
        items[path]->unlock();
        return true;
    }
    return false;
}

// removes all items except the ones in list
void Cache::trimTo(QStringList pathList) {
    for(auto it = items.begin(); it != items.end();) {
        if(!pathList.contains(it.key())) {
            it.value()->lock();
            delete it.value();
            it = items.erase(it);
        } else {
            ++it;
        }
    }
}

const QList<QString> Cache::keys() const {
    return items.keys();
}
