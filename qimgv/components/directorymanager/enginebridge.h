#pragma once

#include <QObject>
#include <QString>

#include <vector>

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
    // Construction is public: the class is instance-safe (each object owns its
    // engine, eventfd and notifier), and tests need isolated bridges — the
    // global instance's watch state would otherwise force every test into
    // unique-directory workarounds. Production keeps using instance().
    explicit EngineBridge(QObject *parent = nullptr);
    ~EngineBridge() override;

    EngineBridge(EngineBridge const &) = delete;
    EngineBridge &operator=(EngineBridge const &) = delete;

    // Accepted asynchronously (returns false only if the engine is dead);
    // results arrive later via dirListed()/openFailed(). sortMode uses the
    // QIMGV_SORT_* codes; the caller maps qimgv's SortingMode explicitly.
    bool openDir(const QString &path, int sortMode, bool showHidden);
    void setSort(int sortMode);
    void setShowHidden(bool show);

    // The directory the reads below currently describe. openDir is accepted
    // asynchronously and the engine swaps its whole listing in one lock when the
    // scan finishes, so between the call and the dirListed event these reads
    // return the PREVIOUS directory — legitimately, and the reason a caller that
    // waits on a count alone can read someone else's listing. Comes from the
    // engine's own event, not from what was requested.
    const QString &listedDirectory() const;

    // synchronous reads of the engine's current listing (for mirroring)
    int fileCount() const;
    int dirCount() const;
    bool entryAt(int index, bool dirs, EngineEntry &out) const;
    // bulk pull: the whole listing serialized in one engine lock pass
    bool exportListing(QByteArray &out, quint32 &fileCount, quint32 &dirCount);
    // Decodes the arena produced by exportListing (wire format documented in
    // model.rs::write_listing). Static and side-effect free so the producer and
    // this consumer can be round-tripped against each other in a test.
    // Returns false unless the record counts and the arena agree exactly —
    // a truncated tail or producer/consumer drift.
    static bool parseListing(const QByteArray &arena, std::vector<EngineEntry> &files,
                             std::vector<EngineEntry> &dirs);

    // Joins the engine's extension whitelist wire format ('\n'-separated).
    // Pure: the formats come in, so tests can pin the seam where an empty
    // list once meant "accept nothing" against the documented NULL contract.
    // An empty result means "accept everything" (qimgv_engine_new's NULL).
    static QByteArray extensionListFor(const QList<QByteArray> &formats);

    static bool supportsWatching();
    // The event-path trace, on when QIMGV_ENGINE_TRACE is set to anything but
    // "0". The engine reads the same variable name on its side, so a line
    // printed here and a line printed there are the same run; the seq in
    // qimgv_event is what lets the two halves be matched up. Shared rather than
    // duplicated because a switch that has to be turned on twice is a switch
    // that gets turned on once.
    static bool traceEnabled();
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
    void dirMutated(int kind, const QString &path, bool isDir, quint64 size, qint64 mtimeMs, int index);
    void thumbNext(const QString &path, int size, bool crop);

private:
    static void notifyCb(void *user);

private slots:
    void drainEvents();

private:
    qimgv_engine *engine = nullptr;
    int eventFd = -1;
    QSocketNotifier *notifier = nullptr;
    quint32 sessionEpoch = 0;
    QString mListedDir;
};
