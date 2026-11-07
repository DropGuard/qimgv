#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QString>
#include <QSize>
#include <QDebug>
#include <QDateTime>
#include <QRegularExpression>
#include <functional>
#include <QVector>

#include <vector>
#include <string>
#include <iostream>
#include <filesystem>
#include <algorithm>

#include <QHash>
#include <QPair>
#include <QSet>

#include "settings.h"
#include "utils/stuff.h"
#include "sourcecontainers/fsentry.h"

class QTimer;
class EngineBridge;

#ifdef Q_OS_WIN32
#include "windows.h"
#endif


class DirectoryManager;

typedef bool (DirectoryManager::*CompareFunction)(const FSEntry &e1, const FSEntry &e2) const;

//TODO: rename? EntrySomething?

class DirectoryManager : public QObject {
    Q_OBJECT
public:
    // Which engine this instance mirrors. Required, not defaulted: a manager
    // that quietly picks the process-wide bridge cannot be pointed at another
    // one, and the ambiguity is not hypothetical — a test that spies on one
    // bridge while its manager talks to a different one sees no events at all and
    // reports that as the design's fault. Production passes
    // EngineBridge::instance() at the construction site, where it is visible.
    explicit DirectoryManager(EngineBridge &bridge);
    // ignored if the same dir is already opened
    bool setDirectory(const QString &);
    // The folder a folder-boundary transition should enter: this folder's place in
    // its parent's order, resolved synchronously because a folder-boundary keypress
    // needs the answer right there. Empty result means there is nowhere to go and
    // the caller shows its boundary message. Never touches the shared engine.
    // mode is the caller listing's own sort mode: the answer has to follow the
    // order the user is looking at, not the persisted setting (a manager can be
    // resorted at runtime without the setting moving)
    static QString adjacentDirectory(const QString &dirPath, bool forward, SortingMode mode);
    // Arms `action` to run once the listing for dirPath lands (mirror
    // rebuilt, counts fresh). One engine session only: a newer openDir
    // supersedes the queue, an open failure drops its own entry, and
    // cancelListed revokes one. For decisions only knowable once the listing
    // exists (Core's folder-boundary load). Returns the cancel token.
    qint64 whenListed(const QString &dirPath, std::function<void()> action);
    bool cancelListed(qint64 token);
    QString directoryPath() const;
    int indexOfFile(const QString &filePath) const;
    int indexOfDir(const QString &dirPath) const;
    QString filePathAt(int index) const;
    unsigned long fileCount() const;
    unsigned long dirCount() const;
    inline bool isSupportedFile(const QString &filePath) const;
    bool isEmpty() const;
    // True when nothing this instance asked the engine for is still in flight:
    // the vectors reflect the directory that was opened, the requested sort is
    // the applied sort, and no resync or bounded rescan is armed. Deliberately
    // answers only "is anything pending", never "what is in the listing" — so a
    // caller can wait on it instead of sleeping, but it can never stand in for
    // an assertion about content (sortingMode() could: it reports the requested
    // mode, which setSortingMode writes before it returns).
    bool isSettled() const;
    // The half of mirroring that applies a listing, split out so a test can hand
    // the mirror one the engine does not currently hold. Nothing in production
    // calls it directly: mirrorEngineListing pulls the arena and forwards. This
    // lets a test describe divergence directly instead of hoping the
    // filesystem produces it on demand.
    bool applyArena(const QString &dirPath, const QByteArray &listing,
                    quint32 newFileCount, quint32 newDirCount);

    bool containsFile(const QString &filePath) const;
    QString fileNameAt(int index) const;
    QString prevOfFile(const QString &filePath) const;
    QString nextOfFile(const QString &filePath) const;
    QDateTime lastModified(const QString &filePath) const;

    QString firstFile() const;
    QString lastFile() const;
    void setSortingMode(SortingMode mode);
    SortingMode sortingMode() const;
    bool isFile(const QString &path) const;
    bool isDir(const QString &path) const;

    unsigned long totalCount() const;
    bool containsDir(const QString &dirPath) const;
    const FSEntry &fileEntryAt(int index) const;
    QString dirPathAt(int index) const;
    QString dirNameAt(int index) const;




    // The one place qimgv's SortingMode becomes a QIMGV_SORT_* code. Public so a
    // test can pin it against the header constants: the two enums are ordered
    // differently, so a positional mistake sorts by the wrong column silently.
    static int engineSortMode(SortingMode mode);

private:
    EngineBridge &mBridge;
    QRegularExpression regex;
    std::vector<FSEntry> fileEntryVec, dirEntryVec;
    const FSEntry defaultEntry;
    QString mDirectoryPath;

    // rust engine mirror (SOURCE_DIRECTORY only)
    QString mEngineDir;      // dir we asked the engine to open
    QString mMirroredDir;    // dir the vectors currently reflect
    SortingMode mAppliedSort;
    QTimer *rescanTimer = nullptr;
    bool mResyncing = false;
    // requested from inside the mirror and consumed after mResyncing clears: a
    // scheduleRescan() call made during mirroring is dropped by its own guard,
    // which is how the engine-behind rescan never used to happen.
    bool mWantRescan = false;
    int mRejectedListings = 0;
    struct PendingListed {
        qint64 token;
        QString dir;
        std::function<void()> action;
    };
    qint64 nextWhenListedToken = 1;
    QVector<PendingListed> pendingOnListed;
    QHash<QString, QPair<quint64, qint64>> mPrevFileMeta; // engine size/mtime per path

    // low-level mirror writes — reserved for the engine event applier; the
    // listing has exactly one writer (the engine) and one reader (the mirror)
    void removeFileEntry(const QString &filePath);
    void removeDirEntry(const QString &dirPath);
    // MODIFIED applied from the wire: the watcher stat'ed before emitting, so
    // the mirror replaces content and splices to the engine's position — it
    // never re-derives either
    void applyEngineModification(const QString &path, bool isDir, quint64 size, qint64 mtimeMs, int index);
    // RENAMED applied from the wire: one row moves instead of two being
    // destroyed and rebuilt, and its pixels survive because a rename changes
    // the name and nothing else. Also the only applier that can answer an
    // overwrite — the engine removed the destination entry, so the mirror drops
    // that row before splicing the source into the engine's position.
    void applyEngineRename(const QString &from, const QString &to, bool isDir, quint64 size, qint64 mtimeMs, int index);
    void scheduleRescan();
    // a landed listing is the definition of agreement: it clears any repair
    // the mirror still owed from an earlier pass
    void markListingInSync();
    // false when the arena was rejected: the mirror keeps what it had, because a
    // truncation applied is a directory that never existed anywhere.
    bool mirrorEngineListing(const QString &dirPath);
    static bool directoryUsable(const QString &dirPath);
    static std::filesystem::file_time_type fileTimeFromMs(qint64 ms);

    void readSettings();
    SortingMode mSortingMode;

    // Directories only ever order by name: the engine's comparator lands on
    // its name tiebreak because scan.rs gives every directory the same
    // size/mtime defaults. The bypass's local sort and the engine's own order
    // both ask for this one rule.

    bool checkFileRange(int index) const;
    bool checkDirRange(int index) const;

private slots:
    void onEngineDirListed(const QString &dirPath);
    void onEngineOpenFailed(const QString &dirPath, int code);
    void onEngineMutated(int kind, const QString &path, bool isDir, quint64 size, qint64 mtimeMs, int index,
                         const QString &oldPath);

signals:
    // The mutation vocabulary of the engine is Added | Modified | Removed |
    // Renamed (watcher/apply.rs::Applied) and these signals mirror exactly that.
    // Renamed is the one the watcher has to earn: the kernel reports a move as
    // MOVED_FROM + MOVED_TO sharing a cookie, and watcher/fd.rs pairs adjacent
    // halves into a single event before it can reach the applier — a pair it
    // cannot pair (split across read batches at a 64 KiB burst) degrades to the
    // Removed + Added these signals would otherwise have carried.
    // The trailing index is the row the source held *after* a destination row
    // this rename overwrote was already dropped, i.e. exactly the position the
    // view holds when the signal fires. Pin: listing_tests liveRenameIsOneEvent.
    void loaded(const QString &path);
    void sortingChanged();
    void loadingFailed(const QString &dirPath);
    void fileRemoved(const QString &filePath, int);
    void fileModified(const QString &filePath);
    void fileAdded(const QString &filePath);
    void fileRenamed(const QString &filePath, const QString &newPath, int oldIndex);

    void dirRemoved(const QString &dirPath, int);
    void dirAdded(const QString &dirPath);
    void dirRenamed(const QString &dirPath, const QString &newPath, int oldIndex);
};
