#include "cacheitem.h"

CacheItem::CacheItem() : sem(1) {
}

CacheItem::CacheItem(std::shared_ptr<Image> _contents) : sem(1) {
    contents = std::move(_contents);
}

std::shared_ptr<Image> CacheItem::getContents() {
    return contents;
}

void CacheItem::lock() {
    sem.acquire(1);
}

void CacheItem::unlock() {
    sem.release(1);
}

int CacheItem::lockStatus() {
    return sem.available();
}
