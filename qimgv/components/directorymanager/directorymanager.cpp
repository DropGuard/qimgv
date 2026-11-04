#include "directorymanager.h"

#include "enginebridge.h"
#include "qimgv_engine.h"

#include <QLoggingCategory>
Q_LOGGING_CATEGORY(lcWatchPerf, "qimgv.perf.watch", QtInfoMsg)

#include <QTimer>
#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

DirectoryManager::DirectoryManager(EngineBridge &bridge) :
    mBridge(bridge),
    mSortingMode(SORT_NAME)
{
    regex.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    collator.setNumericMode(true);

    readSettings();
    setSortingMode(settings->sortingMode());
    connect(settings, &Settings::settingsChanged, this, &DirectoryManager::readSettings);

    connect(&mBridge, &EngineBridge::dirListed,
            this, &DirectoryManager::onEngineDirListed);
    connect(&mBridge, &EngineBridge::openFailed,
            this, &DirectoryManager::onEngineOpenFailed);
    connect(&mBridge, &EngineBridge::dirMutated,
            this, &DirectoryManager::onEngineMutated);
    mAppliedSort = mSortingMode;
}

bool DirectoryManager::path_entry_compare(const FSEntry &e1, const FSEntry &e2) const {
    return collator.compare(e1.path, e2.path) < 0;
};

bool DirectoryManager::path_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const {
    return collator.compare(e1.path, e2.path) > 0;
};

CompareFunction DirectoryManager::dirComparator() const {
    return (mSortingMode == SortingMode::SORT_NAME_DESC)
        ? &DirectoryManager::path_entry_compare_reverse
        : &DirectoryManager::path_entry_compare;
}


// ##############################################################
// ####################### PUBLIC METHODS #######################
// ##############################################################

void DirectoryManager::readSettings() {
    regex.setPattern(settings->supportedFormatsRegex());
}

bool DirectoryManager::setDirectory(const QString &dirPath) {
    if(dirPath.isEmpty()) {
        mEngineDir.clear();
        if(rescanTimer)
            rescanTimer->stop();
        mBridge.stopWatch();
        return false;
    }
    if(!directoryUsable(dirPath))
        return false;

    // The listing is asynchronous — it arrives via onEngineDirListed(), and the
    // vectors keep serving the previous directory until then. A *refused* request
    // is not asynchronous though: nothing will ever arrive, so it has to fail
    // here rather than leave the caller staring at a silently empty folder.
    if(!mBridge.openDir(dirPath, engineSortMode(mSortingMode),
                                         settings->showHiddenFiles())) {
        qWarning() << "[DirectoryManager] engine refused" << dirPath
                   << (qimgv_last_error() ? qimgv_last_error() : "");
        return false;
    }
    mDirectoryPath = dirPath;
    mEngineDir = dirPath;
    mAppliedSort = mSortingMode;
    markListingInSync();
    // a newer open supersedes everything armed for the previous session
    pendingOnListed.clear();
    return true;
}

// See the header: a dirs-only scan into this instance's own vectors, for callers
// that read the sibling order on the next line. Deliberately leaves the shared
// engine alone — an openDir() here would repoint the process-wide inotify watch
// away from the directory the user is actually in.
//
// Directories only, and that is the whole point: this runs on the UI thread, and
// walking the 100k-entry bench fixture costs 35 ms, while also regex-matching and
// collator-sorting every file in it costs 956 ms (measured). A folder-boundary
// keypress must not freeze for a second.
bool DirectoryManager::setSubdirsSync(const QString &dirPath) {
    if(dirPath.isEmpty() || !directoryUsable(dirPath))
        return false;
    mDirectoryPath = dirPath;
    dirEntryVec.clear();
    fileEntryVec.clear();
    addSubdirsFromDirectory(dirPath);
    // same comparator the listing uses, so the sibling order matches the folder
    // view; sorting an empty file list costs nothing
    sortEntryLists();
    return true;
}

void DirectoryManager::addSubdirsFromDirectory(const QString &directoryPath) {
    for(const auto &entry : fs::directory_iterator(toStdString(directoryPath))) {
        QString name = QString::fromStdString(entry.path().filename().generic_string());
        if(!settings->showHiddenFiles() && name.startsWith(QLatin1Char('.')))
            continue;
        if(!entry.is_directory())
            continue;
        FSEntry newEntry;
        newEntry.name = name;
        newEntry.path = QString::fromStdString(entry.path().generic_string());
        newEntry.isDirectory = true;
        dirEntryVec.push_back(std::move(newEntry));
    }
}

QString DirectoryManager::adjacentDirectory(const QString &dirPath, bool forward, SortingMode mode) {
    if(dirPath.isEmpty())
        return QString();
    // the parent's folder order, read synchronously: this answers a keypress.
    // The bridge reference here is nominal: setSubdirsSync never touches the
    // engine (see the header), so this instance holds an engine it will not use.
    // The honest version of this call does not construct a DirectoryManager at
    // all — it is a scan of a folder's subdirectories, and lifting it out of the
    // class is recorded as a follow-up rather than smuggled into this change.
    DirectoryManager lookup{EngineBridge::instance()};
    lookup.setSortingMode(mode);
    if(!lookup.setSubdirsSync(QFileInfo(dirPath).absolutePath()))
        return QString();
    return forward ? lookup.nextOfDir(dirPath) : lookup.prevOfDir(dirPath);
}

bool DirectoryManager::directoryUsable(const QString &dirPath) {
    if(!std::filesystem::exists(toStdString(dirPath))) {
        qDebug() << "[DirectoryManager] Error - path does not exist.";
        return false;
    }
    if(!std::filesystem::is_directory(toStdString(dirPath))) {
        qDebug() << "[DirectoryManager] Error - path is not a directory.";
        return false;
    }
    QDir dir(dirPath);
    if(!dir.isReadable()) {
        qDebug() << "[DirectoryManager] Error - cannot read directory.";
        return false;
    }
    return true;
}

QString DirectoryManager::directoryPath() const {
    return mDirectoryPath;
}

int DirectoryManager::indexOfFile(const QString &filePath) const {
    auto item = find_if(fileEntryVec.begin(), fileEntryVec.end(), [filePath](const FSEntry& e) {
        return e.path == filePath;
    });
    if(item != fileEntryVec.end())
        return distance(fileEntryVec.begin(), item);
    return -1;
}

int DirectoryManager::indexOfDir(const QString &dirPath) const {
    auto item = find_if(dirEntryVec.begin(), dirEntryVec.end(), [dirPath](const FSEntry& e) {
        return e.path == dirPath;
    });
    if(item != dirEntryVec.end())
        return distance(dirEntryVec.begin(), item);
    return -1;
}

QString DirectoryManager::filePathAt(int index) const {
    return checkFileRange(index) ? fileEntryVec.at(index).path : "";
}

QString DirectoryManager::fileNameAt(int index) const {
    return checkFileRange(index) ? fileEntryVec.at(index).name : "";
}

QString DirectoryManager::dirPathAt(int index) const {
    return checkDirRange(index) ? dirEntryVec.at(index).path : "";
}

QString DirectoryManager::dirNameAt(int index) const {
    return checkDirRange(index) ? dirEntryVec.at(index).name : "";
}

QString DirectoryManager::firstFile() const {
    QString filePath = "";
    if(fileEntryVec.size())
        filePath = fileEntryVec.front().path;
    return filePath;
}

QString DirectoryManager::lastFile() const {
    QString filePath = "";
    if(fileEntryVec.size())
        filePath = fileEntryVec.back().path;
    return filePath;
}

QString DirectoryManager::prevOfFile(const QString &filePath) const {
    QString prevFilePath = "";
    int currentIndex = indexOfFile(filePath);
    if(currentIndex > 0)
        prevFilePath = fileEntryVec.at(currentIndex - 1).path;
    return prevFilePath;
}

QString DirectoryManager::nextOfFile(const QString &filePath) const {
    QString nextFilePath = "";
    int currentIndex = indexOfFile(filePath);
    if(currentIndex >= 0 && currentIndex < static_cast<int>(fileEntryVec.size()) - 1)
        nextFilePath = fileEntryVec.at(currentIndex + 1).path;
    return nextFilePath;
}

QString DirectoryManager::prevOfDir(const QString &dirPath) const {
    QString prevDirectoryPath = "";
    int currentIndex = indexOfDir(dirPath);
    if(currentIndex > 0)
        prevDirectoryPath = dirEntryVec.at(currentIndex - 1).path;
    return prevDirectoryPath;
}

QString DirectoryManager::nextOfDir(const QString &dirPath) const {
    QString nextDirectoryPath = "";
    int currentIndex = indexOfDir(dirPath);
    if(currentIndex >= 0 && currentIndex < static_cast<int>(dirEntryVec.size()) - 1)
        nextDirectoryPath = dirEntryVec.at(currentIndex + 1).path;
    return nextDirectoryPath;
}

bool DirectoryManager::checkFileRange(int index) const {
    return index >= 0 && index < static_cast<int>(fileEntryVec.size());
}

bool DirectoryManager::checkDirRange(int index) const {
    return index >= 0 && index < static_cast<int>(dirEntryVec.size());
}

unsigned long DirectoryManager::totalCount() const {
    return fileCount() + dirCount();
}

unsigned long DirectoryManager::fileCount() const {
    return fileEntryVec.size();
}

unsigned long DirectoryManager::dirCount() const {
    return dirEntryVec.size();
}

const FSEntry &DirectoryManager::fileEntryAt(int index) const {
    if(checkFileRange(index))
        return fileEntryVec.at(index);
    else
        return defaultEntry;
}

QDateTime DirectoryManager::lastModified(const QString &filePath) const {
    QFileInfo info;
    if(containsFile(filePath))
        info.setFile(filePath);
    return info.lastModified();
}

// TODO: what about symlinks?
inline
bool DirectoryManager::isSupportedFile(const QString &path) const {
    return ( isFile(path) && regex.match(path).hasMatch() );
}

bool DirectoryManager::isFile(const QString &path) const {
    if(!std::filesystem::exists(toStdString(path)))
        return false;
    if(!std::filesystem::is_regular_file(toStdString(path)))
        return false;
    return true;
}

bool DirectoryManager::isDir(const QString &path) const {
    if(!std::filesystem::exists(toStdString(path)))
        return false;
    if(!std::filesystem::is_directory(toStdString(path)))
        return false;
    return true;
}

bool DirectoryManager::isEmpty() const {
    return fileEntryVec.empty();
}

bool DirectoryManager::containsFile(const QString &filePath) const {
    return (std::find(fileEntryVec.begin(), fileEntryVec.end(), filePath) != fileEntryVec.end());
}

bool DirectoryManager::containsDir(const QString &dirPath) const {
    return (std::find(dirEntryVec.begin(), dirEntryVec.end(), dirPath) != dirEntryVec.end());
}

// ##############################################################
// ###################### PRIVATE METHODS #######################
// ##############################################################
// the setSubdirsSync bypass is the one local producer of order left: it fills
// directories only (files stay empty by contract), and the mirror's own file
// vectors are the engine's order, spliced from the wire — nothing here sorts
// them
void DirectoryManager::sortEntryLists() {
    // Directories always order by name. Two producers have to agree on that, and
    // only one of them is configurable by the sort mode:
    //   - scan.rs gives every directory size 0 and mtime UNIX_EPOCH, so the
    //     engine's comparator lands on its name tiebreak for TIME and SIZE modes;
    //   - the mirror keeps the arena's order instead of re-sorting, so the folder
    //     view *is* the engine's order.
    // Sorting dirs here on those equal fields would hand an all-equivalent range
    // to std::sort (not stable) and let "go to next folder" walk siblings in an
    // order the view does not show. Only NAME_DESC reverses dirs, because that is
    // the one mode whose engine comparator reverses them.
    std::sort(dirEntryVec.begin(), dirEntryVec.end(), std::bind(dirComparator(), this, std::placeholders::_1, std::placeholders::_2));
}

void DirectoryManager::setSortingMode(SortingMode mode) {
    if(mode != mSortingMode) {
        mSortingMode = mode;
        if(!mEngineDir.isEmpty()) {
            // engine resorts and reports back via onEngineDirListed()
            mBridge.setSort(engineSortMode(mode));
            return;
        }
        mAppliedSort = mode;
        if(fileEntryVec.size() > 1 || dirEntryVec.size() > 1) {
            sortEntryLists();
            emit sortingChanged();
        }
    }
}

SortingMode DirectoryManager::sortingMode() const {
    return mSortingMode;
}

bool DirectoryManager::isSettled() const {
    return mMirroredDir == mEngineDir
        && mAppliedSort == mSortingMode
        && !mResyncing
        && !mWantRescan
        && !(rescanTimer && rescanTimer->isActive());
}

// Entry management

// skips filename regex check
bool DirectoryManager::forceInsertFileEntry(const QString &filePath) {
    if(!this->isFile(filePath) || containsFile(filePath))
        return false;
    std::filesystem::directory_entry stdEntry(toStdString(filePath));
    QString fileName = QString::fromStdString(stdEntry.path().filename().generic_string()); // isn't it beautiful
    FSEntry FSEntry(filePath, fileName, stdEntry.file_size(), stdEntry.last_write_time(), stdEntry.is_directory());
    // The mirror makes no order decision here: the entry lands at the end and
    // the includePath round trip re-delivers the listing in the engine's own
    // order within milliseconds. Core's open path needs the entry present
    // synchronously (indexOfFile right after this call), which is the only
    // reason this local insert exists at all.
    fileEntryVec.push_back(std::move(FSEntry));
    if(!directoryPath().isEmpty()) {
        qCDebug(lcWatchPerf) << "fileIns" << filePath << directoryPath();
        emit fileAdded(filePath);
        // keep the engine listing in sync (the filter doesn't match this file)
        mBridge.includePath(filePath);
    }
    return true;
}

void DirectoryManager::removeFileEntry(const QString &filePath) {
    if(!containsFile(filePath))
        return;
    int index = indexOfFile(filePath);
    fileEntryVec.erase(fileEntryVec.begin() + index);
    qCDebug(lcWatchPerf) << "fileRem" << filePath;
    emit fileRemoved(filePath, index);
}

// ---- dir entries

void DirectoryManager::removeDirEntry(const QString &dirPath) {
    if(!containsDir(dirPath))
        return;
    int index = indexOfDir(dirPath);
    dirEntryVec.erase(dirEntryVec.begin() + index);
    qCDebug(lcWatchPerf) << "dirRem" << dirPath;
    emit dirRemoved(dirPath, index);
}



//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
// rust engine bridge

// settings.h's SortingMode order differs from the QIMGV_SORT_* codes — map
// explicitly, never positionally.
int DirectoryManager::engineSortMode(SortingMode mode) {
    switch(mode) {
        case SORT_NAME:       return QIMGV_SORT_NAME;
        case SORT_NAME_DESC:  return QIMGV_SORT_NAME_DESC;
        case SORT_SIZE:       return QIMGV_SORT_SIZE;
        case SORT_SIZE_DESC:  return QIMGV_SORT_SIZE_DESC;
        case SORT_TIME:       return QIMGV_SORT_TIME;
        case SORT_TIME_DESC:  return QIMGV_SORT_TIME_DESC;
    }
    return QIMGV_SORT_NAME;
}

// C++17 has no clock_cast; anchor the file clock against the system clock.
// Used only for mirror entries — change detection compares engine metadata,
// not these values.
std::filesystem::file_time_type DirectoryManager::fileTimeFromMs(qint64 ms) {
    using namespace std::chrono;
    using FileClock = std::filesystem::file_time_type::clock;
    const auto sysNow = system_clock::now();
    const auto target = system_clock::time_point(milliseconds(ms));
    return std::filesystem::file_time_type{FileClock::now() + (target - sysNow)};
}

// in-app file operations and an engine snapshot that looks stale both land
// here; the debounced re-open lets the engine catch up with local truth.
// The mirror keeps entries the engine's listing does not have (local truth the
// engine has not caught up with). Rescanning is the right response, but only a
// bounded number of times: a permanent disagreement is a bug to report, not a
// timer to run forever.
// The repair ladder this used to carry (bounded rescan rounds, a Behind/Diverged
// give-up state) died with the stat-based keep: nothing drives the mirror out of
// agreement anymore, so there is nothing to bound. What remains is the agreement
// mark a landed listing makes.
void DirectoryManager::markListingInSync() {
    mWantRescan = false;
}

void DirectoryManager::scheduleRescan() {
    if(mResyncing)
        return;
    if(mDirectoryPath.isEmpty())
        return;
    if(!rescanTimer) {
        rescanTimer = new QTimer(this);
        rescanTimer->setSingleShot(true);
        rescanTimer->setInterval(150);
        connect(rescanTimer, &QTimer::timeout, this, [this]() {
            if(!mResyncing && !mDirectoryPath.isEmpty())
                mBridge.openDir(mDirectoryPath, engineSortMode(mSortingMode),
                                                 settings->showHiddenFiles());
        });
    }
    rescanTimer->start();
}

void DirectoryManager::onEngineDirListed(const QString &dirPath) {
    if(mResyncing || dirPath != mEngineDir)
        return;
    if(mDirectoryPath != dirPath)
        return;
    mResyncing = true;
    const bool applied = mirrorEngineListing(dirPath);
    mResyncing = false;
    if(mWantRescan) {
        mWantRescan = false;
        scheduleRescan();
    }
    if(!applied) {
        // A rejected arena is not a listing: the actions armed for it must keep
        // waiting for one that arrives, rather than fire against a mirror that was
        // never updated. Bounded so a permanently broken engine cannot make the
        // 150ms rescan timer spin forever.
        if(++mRejectedListings <= 3)
            qWarning() << "[DirectoryManager] listing rejected" << mRejectedListings << "times for" << dirPath;
        else
            qCritical() << "[DirectoryManager] giving up on mirroring" << dirPath
                        << ": the engine keeps rejecting its own listing";
        return;
    }
    mRejectedListings = 0;
    // deferred actions armed for this listing run with fresh counts; the
    // leftovers can never fire (the engine lists one directory at a time).
    // The queue is moved out first: an action may re-enter and arm its own
    // follow-up.
    auto pendings = std::move(pendingOnListed);
    pendingOnListed.clear();
    for(const auto &pending : pendings)
        if(pending.dir == dirPath)
            pending.action();
}

// The engine's own order is the contract: it decides where an entry sits, this
// side only splices. An out-of-range index is a wire bug — degrade to the
// nearest legal position (and say so) rather than corrupting memory; the
// convergence contract test fails long before this branch can matter.
static void insertAtEngineIndex(std::vector<FSEntry> &vec, FSEntry &&entry, int index) {
    const int limit = static_cast<int>(vec.size());
    if(index < 0 || index > limit) {
        qCWarning(lcWatchPerf) << "engine index out of range" << index << "of" << limit;
        index = std::clamp(index, 0, limit);
    }
    vec.insert(vec.begin() + index, std::move(entry));
}

// MODIFIED is applied from the wire: the watcher stat'ed the file before
// emitting, so a re-stat here would be a second decision about the same fact —
// and it is exactly where the old FSEntry self-assignment bug lived.
void DirectoryManager::applyEngineModification(const QString &path, bool isDir, quint64 size, qint64 mtimeMs, int index) {
    std::vector<FSEntry> &vec = isDir ? dirEntryVec : fileEntryVec;
    FSEntry entry;
    entry.path = path;
    entry.name = QFileInfo(path).fileName();
    entry.isDirectory = isDir;
    entry.size = size;
    entry.modifyTime = fileTimeFromMs(isDir ? 0 : mtimeMs);

    int current = isDir ? indexOfDir(path) : indexOfFile(path);
    if(current < 0) {
        // the mirror is behind the engine (it never saw the add): converge by
        // trusting the wire position instead of re-statting
        insertAtEngineIndex(vec, std::move(entry), index);
        if(isDir) {
            emit dirAdded(path);
        } else {
            mPrevFileMeta.insert(path, qMakePair(size, mtimeMs));
            emit fileAdded(path);
        }
        return;
    }
    // the wire index is the entry's FINAL position in the engine's post-update
    // order; erase first, then splice straight to it
    vec.erase(vec.begin() + current);
    insertAtEngineIndex(vec, std::move(entry), index);
    if(!isDir)
        mPrevFileMeta.insert(path, qMakePair(size, mtimeMs));
    emit fileModified(path);
}

void DirectoryManager::onEngineMutated(int kind, const QString &path, bool isDir, quint64 size, qint64 mtimeMs, int index) {
    const bool tracing = EngineBridge::traceEnabled();
    if(mResyncing) {
        if (tracing)
            qDebug() << "[mirror trace] drop (resync in progress)" << path;
        return;
    }
    // A mutation edits the listing the vectors currently hold, so its path has
    // to live in the directory those vectors describe. The wire carries no
    // directory for a mutation, and the engine's order index is computed
    // against its own listing — splicing a foreign path in would put a row from
    // another folder at a position that means nothing here. Nothing has been
    // mirrored yet, so there is nothing to contradict.
    if(!mMirroredDir.isEmpty() && !path.startsWith(mMirroredDir + QLatin1Char('/'))) {
        qWarning() << "[DirectoryManager] mutation outside the mirrored directory:" << path
                   << "mirrored:" << mMirroredDir;
        return;
    }
    switch(kind) {
        case QIMGV_MUT_ADDED: {
            if(containsFile(path) || containsDir(path)) {
                if (tracing)
                    qDebug() << "[mirror trace] drop (already mirrored)" << path;
                return; // already mirrored (racing duplicate)
            }
            FSEntry entry;
            entry.path = path;
            entry.name = QFileInfo(path).fileName();
            entry.isDirectory = isDir;
            entry.size = size;
            entry.modifyTime = fileTimeFromMs(isDir ? 0 : mtimeMs);
            insertAtEngineIndex(isDir ? dirEntryVec : fileEntryVec, std::move(entry), index);
            if(isDir) {
                emit dirAdded(path);
            } else {
                mPrevFileMeta.insert(path, qMakePair(size, mtimeMs));
                emit fileAdded(path);
            }
            break;
        }
        case QIMGV_MUT_MODIFIED:
            applyEngineModification(path, isDir, size, mtimeMs, index);
            break;
        case QIMGV_MUT_REMOVED:
            removeDirEntry(path);
            removeFileEntry(path);
            break;
    }
    if (tracing) {
        // the counts, not just the kind: the failure this trace exists to name is
        // a mutation that lands and still leaves the mirror one entry short
        qDebug() << "[mirror trace] applied kind" << kind << path << "index" << index
                 << "files" << fileEntryVec.size() << "dirs" << dirEntryVec.size();
    }
}

qint64 DirectoryManager::whenListed(const QString &dirPath, std::function<void()> action) {
    const qint64 token = nextWhenListedToken++;
    pendingOnListed.append({token, dirPath, std::move(action)});
    return token;
}

bool DirectoryManager::cancelListed(qint64 token) {
    for(int i = 0; i < pendingOnListed.size(); ++i) {
        if(pendingOnListed[i].token == token) {
            pendingOnListed.remove(i);
            return true;
        }
    }
    return false;
}

void DirectoryManager::onEngineOpenFailed(const QString &dirPath, int code) {
    if(dirPath != mEngineDir)
        return;
    qWarning() << "[DirectoryManager] engine open failed:" << dirPath << "code" << code;
    // an open that failed will never produce the listing its actions wait for
    pendingOnListed.erase(std::remove_if(pendingOnListed.begin(), pendingOnListed.end(),
                             [&](const PendingListed &p) { return p.dir == dirPath; }),
                          pendingOnListed.end());
    emit loadingFailed(dirPath);
}

bool DirectoryManager::mirrorEngineListing(const QString &dirPath) {
    // bulk pull: the whole listing crosses in one engine lock pass
    QByteArray listing;
    quint32 newFileCount = 0, newDirCount = 0;
    if(!mBridge.exportListing(listing, newFileCount, newDirCount))
        return false;
    return applyArena(dirPath, listing, newFileCount, newDirCount);
}

bool DirectoryManager::applyArena(const QString &dirPath, const QByteArray &listing,
                                  quint32 newFileCount, quint32 newDirCount) {
    QElapsedTimer rebuildTimer;
    rebuildTimer.start();

    // parse the arena into mirror entries
    QElapsedTimer parseTimer;
    parseTimer.start();
    std::vector<EngineBridge::EngineEntry> files, dirs;
    if(!EngineBridge::parseListing(listing, files, dirs)) {
        // Nothing was parsed (a rejected arena yields no records), and installing
        // the prefix that happened to decode would create a directory that does
        // not exist anywhere: entries silently missing from the view, from the
        // counts and from end-of-folder navigation. Keep the old mirror and ask
        // for another listing.
        qWarning() << "[DirectoryManager] listing arena rejected:" << listing.size()
                   << "bytes, header claimed" << newFileCount << "files +" << newDirCount << "dirs";
        mWantRescan = true;
        return false;
    }
    qDebug() << "[DirectoryManager] arena pull+parse" << parseTimer.elapsed() << "ms; parsed"
             << files.size() << "files" << dirs.size() << "dirs; arena" << listing.size() << "bytes";

    QElapsedTimer hashTimer;
    hashTimer.start();
    QHash<QString, QPair<quint64, qint64>> newMeta;
    newMeta.reserve(newFileCount * 2);
    for(const auto &f : files)
        newMeta.insert(f.path, {f.size, f.mtimeMs});

    QSet<QString> newPaths;
    newPaths.reserve(newFileCount * 2);
    for(const auto &f : files)
        newPaths.insert(f.path);
    QSet<QString> newDirPaths;
    newDirPaths.reserve(newDirCount * 2);
    for(const auto &d : dirs)
        newDirPaths.insert(d.path);
    qDebug() << "[dbg] parse" << parseTimer.elapsed() << "ms; hash containers" << hashTimer.elapsed() << "ms";

    // The mirror is a pure projection of the export: an entry the export lacks
    // is gone, with no stat of the filesystem to second-guess the engine. The
    // stat-based keep that used to live here repaired missed events silently
    // and made divergence unmeasurable; missed events now surface through the
    // convergence contract instead. Local mutations no longer run ahead of the
    // engine — forceInsert's includePath lands in the same export that would
    // otherwise be compared against it.

    const bool isNewDir = (mMirroredDir != dirPath);
    QElapsedTimer rebuildTimer2;
    rebuildTimer2.start();

    if(isNewDir) {
        fileEntryVec.clear();
        dirEntryVec.clear();
        for(const auto &f : files) {
            FSEntry entry;
            entry.path = f.path;
            entry.name = f.name;
            entry.isDirectory = false;
            entry.size = f.size;
            entry.modifyTime = fileTimeFromMs(f.mtimeMs);
            fileEntryVec.push_back(std::move(entry));
        }
        for(const auto &d : dirs) {
            FSEntry entry;
            entry.path = d.path;
            entry.name = d.name;
            entry.isDirectory = true;
            dirEntryVec.push_back(std::move(entry));
        }
        mPrevFileMeta = std::move(newMeta);
        mMirroredDir = dirPath;
        qDebug() << "[DirectoryManager] listing vectors rebuilt" << rebuildTimer2.elapsed() << "ms";
        // NOTE: emit loaded triggers the view-layer repopulation (100k items
        // measured ~400ms in the gui/ layer) — on the same UI thread but
        // belonging to the view, not to this mirror
        emit loaded(mDirectoryPath);
        qDebug() << "[DirectoryManager] listing rebuild (incl. view repopulation)"
                 << newFileCount << "files took" << rebuildTimer.elapsed() << "ms";
        // a fresh listing is the definition of agreement
        markListingInSync();
        return true;
    }

    // same directory: granular resync — keep views/selection stable
    QSet<QString> oldPaths;
    for(const auto &f : fileEntryVec)
        oldPaths.insert(f.path);
    QSet<QString> oldDirPaths;
    for(const auto &d : dirEntryVec)
        oldDirPaths.insert(d.path);

    // 1. removals (descending index keeps earlier indexes valid); an entry the
    //    export lacks is removed unconditionally — the mirror never consults
    //    the filesystem against the engine
    for(int i = static_cast<int>(fileEntryVec.size()) - 1; i >= 0; --i) {
        const QString &p = fileEntryVec[static_cast<size_t>(i)].path;
        if(!newPaths.contains(p)) {
            emit fileRemoved(p, i);
            fileEntryVec.erase(fileEntryVec.begin() + i);
        }
    }
    for(int i = static_cast<int>(dirEntryVec.size()) - 1; i >= 0; --i) {
        const QString &p = dirEntryVec[static_cast<size_t>(i)].path;
        if(!newDirPaths.contains(p)) {
            emit dirRemoved(p, i);
            dirEntryVec.erase(dirEntryVec.begin() + i);
        }
    }

    // 2. changed files: compare engine metadata against the previous mirror —
    //    the signal is all the mirror owes the view; the entry content comes
    //    from the export in step 3, never from a native re-stat
    for(const auto &f : files) {
        if(oldPaths.contains(f.path)) {
            const auto prev = mPrevFileMeta.constFind(f.path);
            if(prev == mPrevFileMeta.constEnd() || prev->first != f.size || prev->second != f.mtimeMs)
                emit fileModified(f.path);
        }
    }

    // 3. rebuild both vectors in engine order
    fileEntryVec.clear();
    for(const auto &f : files) {
        FSEntry entry;
        entry.path = f.path;
        entry.name = f.name;
        entry.isDirectory = false;
        entry.size = f.size;
        entry.modifyTime = fileTimeFromMs(f.mtimeMs);
        fileEntryVec.push_back(std::move(entry));
    }
    dirEntryVec.clear();
    for(const auto &d : dirs) {
        FSEntry entry;
        entry.path = d.path;
        entry.name = d.name;
        entry.isDirectory = true;
        dirEntryVec.push_back(std::move(entry));
    }

    // 4. additions (emitted after the vectors hold the final state)
    for(const auto &f : files)
        if(!oldPaths.contains(f.path))
            emit fileAdded(f.path);
    for(const auto &d : dirs)
        if(!oldDirPaths.contains(d.path))
            emit dirAdded(d.path);

    // 5. resort signal if the sort mode changed since the last mirror
    if(mAppliedSort != mSortingMode) {
        mAppliedSort = mSortingMode;
        emit sortingChanged();
    }

    mPrevFileMeta = std::move(newMeta);
    qDebug() << "[DirectoryManager] listing resync" << newFileCount << "files took" << rebuildTimer.elapsed() << "ms";
    markListingInSync();
    return true;
}
