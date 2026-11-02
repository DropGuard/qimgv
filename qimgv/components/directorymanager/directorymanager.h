#pragma once

#include <QObject>
#include <QCollator>
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

#ifdef Q_OS_WIN32
#include "windows.h"
#endif


class DirectoryManager;

typedef bool (DirectoryManager::*CompareFunction)(const FSEntry &e1, const FSEntry &e2) const;

//TODO: rename? EntrySomething?

class DirectoryManager : public QObject {
    Q_OBJECT
public:
    DirectoryManager();
    // ignored if the same dir is already opened
    bool setDirectory(QString);
    // Blocking SUBDIRECTORIES-only scan into this instance, for callers that read
    // the sibling order on the very next line (Core's folder-boundary navigation).
    // setDirectory() cannot serve them: its listing is asynchronous, so the read
    // would see an empty mirror. Files stay empty on purpose — this runs on the UI
    // thread and only needs the folder order.
    bool setSubdirsSync(const QString &dirPath);
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
    int indexOfFile(QString filePath) const;
    int indexOfDir(QString dirPath) const;
    QString filePathAt(int index) const;
    unsigned long fileCount() const;
    unsigned long dirCount() const;
    inline bool isSupportedFile(QString filePath) const;
    bool isEmpty() const;
    bool containsFile(QString filePath) const;
    QString fileNameAt(int index) const;
    QString prevOfFile(QString filePath) const;
    QString nextOfFile(QString filePath) const;
    QString prevOfDir(QString filePath) const;
    QString nextOfDir(QString filePath) const;
    void sortEntryLists();
    QDateTime lastModified(QString filePath) const;

    QString firstFile() const;
    QString lastFile() const;
    void setSortingMode(SortingMode mode);
    SortingMode sortingMode() const;
    bool isFile(QString path) const;
    bool isDir(QString path) const;

    unsigned long totalCount() const;
    bool containsDir(QString dirPath) const;
    const FSEntry &fileEntryAt(int index) const;
    QString dirPathAt(int index) const;
    QString dirNameAt(int index) const;

    bool forceInsertFileEntry(const QString &filePath);

    //bool forceInsertDirEntry(const QString &dirPath);
    //void updateDirEntry(const QString &dirPath);

    bool listingOpen() const;

    QStringList fileList() const;

    // The one place qimgv's SortingMode becomes a QIMGV_SORT_* code. Public so a
    // test can pin it against the header constants: the two enums are ordered
    // differently, so a positional mistake sorts by the wrong column silently.
    static int engineSortMode(SortingMode mode);

private:
    QRegularExpression regex;
    QCollator collator;
    std::vector<FSEntry> fileEntryVec, dirEntryVec;
    const FSEntry defaultEntry;
    QString mDirectoryPath;

    // rust engine mirror (SOURCE_DIRECTORY only)
    QString mEngineDir;      // dir we asked the engine to open
    QString mMirroredDir;    // dir the vectors currently reflect
    SortingMode mAppliedSort;
    QTimer *rescanTimer = nullptr;
    bool mResyncing = false;
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
    void updateFileEntry(const QString &filePath);
    void scheduleRescan();
    void mirrorEngineListing(const QString &dirPath);
    static bool directoryUsable(const QString &dirPath);
    static std::filesystem::file_time_type fileTimeFromMs(qint64 ms);

    void readSettings();
    SortingMode mSortingMode;
    // true once an entry list exists in this instance (engine-opened or the
    // local dirs-only scan); replaces a one-value enum
    bool mListOpen = false;

    bool path_entry_compare(const FSEntry &e1, const FSEntry &e2) const;
    bool path_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const;
    bool name_entry_compare(const FSEntry &e1, const FSEntry &e2) const;
    bool name_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const;
    bool date_entry_compare(const FSEntry &e1, const FSEntry &e2) const;
    bool date_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const;
    CompareFunction compareFunction();
    // Directories only ever order by name: compareFunction() can pick a time or
    // size comparator, and those are meaningless for directories because scan.rs
    // gives every one of them the same size/mtime defaults. Sorting and inserting
    // have to use the same rule, so both ask for this one.
    CompareFunction dirComparator() const;
    bool size_entry_compare(const FSEntry &e1, const FSEntry &e2) const;
    bool size_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const;

    void addSubdirsFromDirectory(const QString &directoryPath);
    bool checkFileRange(int index) const;
    bool checkDirRange(int index) const;

private slots:
    void onEngineDirListed(QString dirPath);
    void onEngineOpenFailed(QString dirPath, int code);
    void onEngineMutated(int kind, QString path, bool isDir, quint64 size, qint64 mtimeMs);

signals:
    void loaded(const QString &path);
    void sortingChanged();
    void loadingFailed(QString dirPath);
    void fileRemoved(QString filePath, int);
    void fileModified(QString filePath);
    void fileAdded(QString filePath);
    void fileRenamed(QString fromPath, int indexFrom, QString toPath, int indexTo);

    void dirRemoved(QString dirPath, int);
    void dirAdded(QString dirPath);
    void dirRenamed(QString fromPath, int indexFrom, QString toPath, int indexTo);
};
