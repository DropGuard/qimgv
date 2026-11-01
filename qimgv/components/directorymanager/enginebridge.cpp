#include "enginebridge.h"

#include <QSocketNotifier>
#include <QDebug>
#include <cerrno>
#include <cstring>

#include "qimgv_engine.h"
#include "settings.h"

#include <sys/eventfd.h>
#include <unistd.h>

namespace {

// '\n'-joined extension whitelist, sourced from the deployed QImageReader
// plugins — the same list Settings::supportedFormatsRegex() builds its
// pattern from. Empty means "accept everything" (FFI contract: NULL).
QByteArray extensionList() {
    if (!settings)
        return QByteArray();
    QStringList out;
    const auto formats = settings->supportedFormats();
    for (const auto &f : formats)
        out << QString::fromUtf8(f);
    return out.join('\n').toUtf8();
}

} // namespace

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

EngineBridge::EngineBridge() {
    eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd == -1) {
        qWarning() << "[EngineBridge] eventfd failed:" << std::strerror(errno);
        return;
    }
    const QByteArray exts = extensionList();
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
        char buf[4096];
        const int r = qimgv_engine_take_event(engine, &ev, buf, sizeof(buf), &mut);
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
        } else if (ev.kind == QIMGV_EVENT_OPEN_FAILED) {
            qWarning() << "[EngineBridge] open failed:" << dir << "code" << ev.code
                       << (qimgv_last_error() ? qimgv_last_error() : "");
            emit openFailed(dir, ev.code);
        }
    }
}
