#include "enginebridge.h"

#include <QSocketNotifier>
#include <QDebug>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <cstring>

#include "qimgv_engine.h"
#include "settings.h"

#include <sys/eventfd.h>
#include <unistd.h>

QByteArray EngineBridge::extensionListFor(const QList<QByteArray> &formats) {
    // the '\n'-joined extension whitelist for the engine, sourced from the
    // deployed QImageReader plugins — the same list
    // Settings::supportedFormatsRegex() builds its pattern from. Empty means
    // "accept everything" (FFI contract: NULL).
    QStringList out;
    for (const auto &f : formats)
        out << QString::fromUtf8(f);
    return out.join('\n').toUtf8();
}

void EngineBridge::notifyCb(void *user) {
    // Runs on an engine thread. eventfd writes coalesce; the read side drains.
    const uint64_t one = 1;
    ssize_t rc;
    do {
        rc = ::write(*static_cast<int *>(user), &one, sizeof(one));
    } while (rc == -1 && errno == EINTR);
    (void)rc;
}

EngineBridge &EngineBridge::instance() {
    static EngineBridge bridge;
    return bridge;
}

EngineBridge::EngineBridge(QObject *parent) : QObject(parent) {
    eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd == -1) {
        qWarning() << "[EngineBridge] eventfd failed:" << std::strerror(errno);
        return;
    }
    const QByteArray exts = settings ? extensionListFor(settings->supportedFormats())
                                     : QByteArray();
    engine = qimgv_engine_new(&EngineBridge::notifyCb, &eventFd, 2,
                              exts.isEmpty() ? nullptr : exts.constData());
    if (!engine) {
        const char *err = qimgv_last_error();
        qWarning() << "[EngineBridge] engine init failed:" << (err ? err : "unknown");
        return;
    }
    qDebug() << "[EngineBridge] ctor done, eventFd:" << eventFd;
    notifier = new QSocketNotifier(eventFd, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated,
            this, [this]() { drainEvents(); });
}

EngineBridge::~EngineBridge() {
    delete notifier;
    if (engine)
        qimgv_engine_free(engine);
    if (eventFd != -1)
        ::close(eventFd);
}

bool EngineBridge::openDir(const QString &path, int sortMode, bool showHidden) {
    if (!engine)
        return false;
    setShowHidden(showHidden);
    // sort rides with OpenDir (atomic in the engine) — a separate setSort
    // would emit a stale resort event for the previous directory's state
    const QByteArray utf8 = path.toUtf8();
    qDebug() << "[EngineBridge] openDir" << path;
    uint32_t epoch = 0;
    const int rc = qimgv_engine_open_dir(engine, utf8.constData(), sortMode, &epoch);
    if (rc == QIMGV_OK)
        sessionEpoch = epoch; // events from older sessions are stale
    return rc == QIMGV_OK;
}

void EngineBridge::setSort(int sortMode) {
    if (engine)
        qimgv_engine_set_sort(engine, sortMode);
}

void EngineBridge::setShowHidden(bool show) {
    if (engine)
        qimgv_engine_set_show_hidden(engine, show ? 1 : 0);
}

int EngineBridge::fileCount() const {
    if (!engine)
        return 0;
    const int n = qimgv_engine_file_count(engine);
    return n < 0 ? 0 : n;
}

int EngineBridge::dirCount() const {
    if (!engine)
        return 0;
    const int n = qimgv_engine_dir_count(engine);
    return n < 0 ? 0 : n;
}

bool EngineBridge::supportsWatching() {
    return qimgv_engine_supports_watching() != 0;
}

bool EngineBridge::includePath(const QString &path) {
    if (!engine)
        return false;
    const QByteArray utf8 = path.toUtf8();
    return qimgv_engine_include_path(engine, utf8.constData()) == QIMGV_OK;
}

void EngineBridge::stopWatch() {
    if (engine)
        qimgv_engine_stop_watch(engine);
}

bool EngineBridge::thumbRequest(const QString &path, int size, bool crop, bool replaceBatch) {
    if (!engine)
        return false;
    const QByteArray utf8 = path.toUtf8();
    return qimgv_engine_thumb_request(engine, utf8.constData(),
                                      static_cast<uint32_t>(size),
                                      crop ? 1 : 0, replaceBatch ? 1 : 0) == QIMGV_OK;
}

void EngineBridge::thumbDone(const QString &path, int size, bool crop) {
    if (!engine)
        return;
    const QByteArray utf8 = path.toUtf8();
    qimgv_engine_thumb_done(engine, utf8.constData(),
                            static_cast<uint32_t>(size), crop ? 1 : 0);
}

void EngineBridge::thumbClear() {
    if (engine)
        qimgv_engine_thumb_clear(engine);
}

void EngineBridge::thumbConfig(int maxInFlight) {
    if (engine)
        qimgv_engine_thumb_config(engine, static_cast<uint32_t>(maxInFlight));
}

bool EngineBridge::exportListing(QByteArray &out, quint32 &fileCount, quint32 &dirCount) {
    if (!engine)
        return false;
    uint32_t needed = 0, fc = 0, dc = 0;
    if (qimgv_engine_export_listing(engine, nullptr, 0, &needed, &fc, &dc) == QIMGV_OK) {
        out.clear(); // empty listing
        fileCount = fc;
        dirCount = dc;
        return true;
    }
    out.resize(int(needed));
    if (qimgv_engine_export_listing(engine, reinterpret_cast<unsigned char *>(out.data()),
                                    size_t(out.size()), &needed, &fc, &dc) != QIMGV_OK) {
        qWarning() << "[EngineBridge] export listing failed:" << qimgv_last_error();
        return false;
    }
    fileCount = fc;
    dirCount = dc;
    return true;
}

// Decodes the arena written by DirectoryState::write_listing (model.rs).
// Little-endian: u32 file_count, u32 dir_count, then file_count + dir_count
// records of u64 handle, u64 size, i64 mtime_ms, u32 name_len, u32 path_len,
// name bytes, path bytes (no NUL) — files first. The handle is skipped: the
// mirror keys on path. A record that does not fit ends the parse early, so a
// truncated arena yields a short listing rather than a crash.
//
// Returns false unless the arena held exactly file_count file records plus
// dir_count dir records and was consumed to the last byte. Both checks matter:
// the header counts are the only place where producer and consumer agree on how
// much data SHOULD be here, so a disagreeing count is drift, not an empty list.
bool EngineBridge::parseListing(const QByteArray &arena, std::vector<EngineEntry> &files,
                                std::vector<EngineEntry> &dirs) {
    if (arena.size() < 8)
        return false;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(arena.constData());
    const unsigned char *end = p + arena.size();
    auto rd32 = [&p]() -> quint32 { quint32 v = qFromLittleEndian<quint32>(p); p += 4; return v; };
    auto rd64 = [&p]() -> quint64 { quint64 v = qFromLittleEndian<quint64>(p); p += 8; return v; };

    const quint32 fc = rd32();
    const quint32 dc = rd32();
    files.clear();
    dirs.clear();
    // cap the reservation by the byte budget: a header that disagrees with the
    // arena must not drive a huge allocation
    const size_t maxRecords = size_t(arena.size()) / 32;
    files.reserve(std::min<size_t>(fc, maxRecords));
    dirs.reserve(std::min<size_t>(dc, maxRecords));

    auto readRun = [&](quint32 count, bool isDir) {
        for (quint32 i = 0; i < count && p + 32 <= end; ++i) {
            p += 8; // handle
            const quint64 size = rd64();
            const qint64 mtimeMs = static_cast<qint64>(rd64());
            const quint32 nlen = rd32();
            const quint32 plen = rd32();
            if (p + nlen + plen > end)
                break;
            EngineEntry e;
            e.name = QString::fromUtf8(reinterpret_cast<const char *>(p), int(nlen));
            p += nlen;
            e.path = QString::fromUtf8(reinterpret_cast<const char *>(p), int(plen));
            p += plen;
            e.size = size;
            e.mtimeMs = mtimeMs;
            e.isDir = isDir;
            (isDir ? dirs : files).push_back(std::move(e));
        }
    };
    readRun(fc, false);
    readRun(dc, true);
    return p == end && files.size() == fc && dirs.size() == dc;
}

bool EngineBridge::entryAt(int index, bool dirs, EngineEntry &out) const {
    if (!engine || index < 0)
        return false;
    qimgv_entry_meta meta{};
    char name[512], path[4096];
    const int r = dirs
        ? qimgv_engine_dir_entry_at(engine, index, &meta, name, sizeof(name), path, sizeof(path))
        : qimgv_engine_file_entry_at(engine, index, &meta, name, sizeof(name), path, sizeof(path));
    if (r != QIMGV_OK)
        return false;
    out.name = QString::fromUtf8(name);
    out.path = QString::fromUtf8(path);
    out.size = meta.size;
    out.mtimeMs = meta.mtime_ms;
    out.isDir = meta.is_dir != 0;
    return true;
}

void EngineBridge::drainEvents() {
    // clear the wake counter (multiple notifies may have coalesced)
    uint64_t val = 0;
    while (::read(eventFd, &val, sizeof(val)) == static_cast<ssize_t>(sizeof(val))) {
    }

    if (!engine)
        return;
    for (;;) {
        qimgv_event ev{};
        qimgv_mutation mut{};
        qimgv_thumb_req treq{};
        char buf[4096];
        const int r = qimgv_engine_take_event(engine, &ev, buf, sizeof(buf), &mut, &treq);
        if (r <= 0) {
            if (r < 0)
                qWarning() << "[EngineBridge] take_event failed:" << qimgv_last_error();
            break;
        }
        if (sessionEpoch != 0 && ev.epoch != 0 && ev.epoch < sessionEpoch)
            continue; // event from an older session (dead receiver) — drop
        const QString dir = QString::fromUtf8(buf);
        if (ev.kind == QIMGV_EVENT_DIR_LISTED) {
            emit dirListed(dir);
        } else if (ev.kind == QIMGV_EVENT_DIR_MUTATED) {
            emit dirMutated(mut.kind, dir, mut.is_dir != 0, mut.size, mut.mtime_ms);
        } else if (ev.kind == QIMGV_EVENT_THUMB_NEXT) {
            emit thumbNext(dir, static_cast<int>(treq.size), treq.crop != 0);
        } else if (ev.kind == QIMGV_EVENT_OPEN_FAILED) {
            qWarning() << "[EngineBridge] open failed:" << dir << "code" << ev.code
                       << (qimgv_last_error() ? qimgv_last_error() : "");
            emit openFailed(dir, ev.code);
        }
    }
}
