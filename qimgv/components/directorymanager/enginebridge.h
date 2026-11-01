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

signals:
    void dirListed(const QString &dirPath);
    void openFailed(const QString &dirPath, int statusCode);

private:
    EngineBridge();
    static void notifyCb(void *user);

private slots:
    void drainEvents();

private:
    qimgv_engine *engine = nullptr;
    int eventFd = -1;
    QSocketNotifier *notifier = nullptr;
};
