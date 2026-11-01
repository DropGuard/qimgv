#pragma once

#include <QObject>
#include <QString>

struct qimgv_engine;
class QSocketNotifier;

// Owns the process-wide Rust engine instance and pumps its events into the
// Qt event loop. The engine's notify callback writes to an eventfd; a
// QSocketNotifier wakes the UI thread, which drains qimgv_engine_take_event.
// Engine calls never block the UI thread.
class EngineBridge : public QObject {
    Q_OBJECT
public:
    struct EngineEntry {
        QString name, path;
        quint64 size = 0;
        qint64 mtimeMs = 0;
        bool isDir = false;
    };

    static EngineBridge &instance();
    ~EngineBridge() override;

    EngineBridge(EngineBridge const &) = delete;
    EngineBridge &operator=(EngineBridge const &) = delete;

    // Accepted asynchronously (returns false only if the engine is dead);
    // results arrive later via dirListed()/openFailed(). sortMode uses the
    // QIMGV_SORT_* codes; the caller maps qimgv's SortingMode explicitly.
    bool openDir(const QString &path, int sortMode, bool showHidden);
    void setSort(int sortMode);
    void setShowHidden(bool show);

    // synchronous reads of the engine's current listing (for mirroring)
    int fileCount() const;
    int dirCount() const;
    bool entryAt(int index, bool dirs, EngineEntry &out) const;
    // bulk pull: the whole listing serialized in one engine lock pass
    bool exportListing(QByteArray &out, quint32 &fileCount, quint32 &dirCount);

    static bool supportsWatching();
    bool includePath(const QString &path);
    void stopWatch();

    // thumbnail scheduling (viewport priority lives in the engine)
    bool thumbRequest(const QString &path, int size, bool crop, bool replaceBatch);
    void thumbDone(const QString &path, int size, bool crop);
    void thumbClear();
    void thumbConfig(int maxInFlight);

signals:
    void dirListed(const QString &dirPath);
    void openFailed(const QString &dirPath, int statusCode);
    void dirMutated(int kind, const QString &path, bool isDir, quint64 size, qint64 mtimeMs);
    void thumbNext(const QString &path, int size, bool crop);

private:
    EngineBridge();
    static void notifyCb(void *user);

private slots:
    void drainEvents();

private:
    qimgv_engine *engine = nullptr;
    int eventFd = -1;
    QSocketNotifier *notifier = nullptr;
    quint32 sessionEpoch = 0;
};
