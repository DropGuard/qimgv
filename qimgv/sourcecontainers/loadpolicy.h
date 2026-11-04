#pragma once

#include <QMultiMap>
#include <QString>

#include "settings.h"

// The environment a decode or classification runs in, captured on the UI
// thread at the moment the request was made and carried to the worker.
// Replaces off-thread reads of the settings global (R4): pool threads receive
// values, they do not reach for ambient state. fromSettings() is the capture
// form and is only legal on the thread that owns the settings global — the
// dispatch site, never the pool.
struct LoadPolicy {
    bool jxlAnimation = false;
    bool videoPlayback = false;
    QMultiMap<QByteArray, QByteArray> videoFormats;
    QString mpvBinary;
    QString tmpDir;
    int allocationLimitMb = 64; // QImageReader's own default

    static LoadPolicy fromSettings() {
        LoadPolicy p;
        p.jxlAnimation = settings->jxlAnimation();
        p.videoPlayback = settings->videoPlayback();
        p.videoFormats = settings->videoFormats();
        p.mpvBinary = settings->mpvBinary();
        p.tmpDir = settings->tmpDir();
        p.allocationLimitMb = settings->memoryAllocationLimit();
        return p;
    }
};
