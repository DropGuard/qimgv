#include "directorymanager.h"

#include "enginebridge.h"
#include "qimgv_engine.h"

#include <QLoggingCategory>
Q_LOGGING_CATEGORY(lcWatchPerf, "qimgv.perf.watch", QtInfoMsg)

#include <QTimer>
#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

DirectoryManager::DirectoryManager() :
    mSortingMode(SORT_NAME)
{
    regex.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    collator.setNumericMode(true);

    readSettings();
    setSortingMode(settings->sortingMode());
    connect(settings, &Settings::settingsChanged, this, &DirectoryManager::readSettings);

    connect(&EngineBridge::instance(), &EngineBridge::dirListed,
            this, &DirectoryManager::onEngineDirListed);
    connect(&EngineBridge::instance(), &EngineBridge::openFailed,
            this, &DirectoryManager::onEngineOpenFailed);
    connect(&EngineBridge::instance(), &EngineBridge::dirMutated,
            this, &DirectoryManager::onEngineMutated);
    mAppliedSort = mSortingMode;
}

template< typename T, typename Pred >
typename std::vector<T>::iterator
insert_sorted(std::vector<T> & vec, T const& item, Pred pred) {
    return vec.insert(std::upper_bound(vec.begin(), vec.end(), item, pred), item);
}

bool DirectoryManager::path_entry_compare(const FSEntry &e1, const FSEntry &e2) const {
    return collator.compare(e1.path, e2.path) < 0;
};

bool DirectoryManager::path_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const {
    return collator.compare(e1.path, e2.path) > 0;
};

bool DirectoryManager::name_entry_compare(const FSEntry &e1, const FSEntry &e2) const {
    return collator.compare(e1.name, e2.name) < 0;
};

bool DirectoryManager::name_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const {
    return collator.compare(e1.name, e2.name) > 0;
};

bool DirectoryManager::date_entry_compare(const FSEntry& e1, const FSEntry& e2) const {
    return e1.modifyTime < e2.modifyTime;
}

bool DirectoryManager::date_entry_compare_reverse(const FSEntry& e1, const FSEntry& e2) const {
    return e1.modifyTime > e2.modifyTime;
}

bool DirectoryManager::size_entry_compare(const FSEntry& e1, const FSEntry& e2) const {
    return e1.size < e2.size;
}

bool DirectoryManager::size_entry_compare_reverse(const FSEntry& e1, const FSEntry& e2) const {
    return e1.size > e2.size;
}

CompareFunction DirectoryManager::dirComparator() const {
    return (mSortingMode == SortingMode::SORT_NAME_DESC)
        ? &DirectoryManager::path_entry_compare_reverse
        : &DirectoryManager::path_entry_compare;
}

CompareFunction DirectoryManager::compareFunction() {
    CompareFunction cmpFn = &DirectoryManager::path_entry_compare;
    if(mSortingMode == SortingMode::SORT_NAME_DESC)
        cmpFn = &DirectoryManager::path_entry_compare_reverse;
    if(mSortingMode == SortingMode::SORT_TIME)
        cmpFn = &DirectoryManager::date_entry_compare;
    if(mSortingMode == SortingMode::SORT_TIME_DESC)
        cmpFn = &DirectoryManager::date_entry_compare_reverse;
    if(mSortingMode == SortingMode::SORT_SIZE)
        cmpFn = &DirectoryManager::size_entry_compare;
    if(mSortingMode == SortingMode::SORT_SIZE_DESC)
        cmpFn = &DirectoryManager::size_entry_compare_reverse;
    return cmpFn;
}


// ##############################################################
// ####################### PUBLIC METHODS #######################
// ##############################################################

void DirectoryManager::readSettings() {
    regex.setPattern(settings->supportedFormatsRegex());
}

bool DirectoryManager::setDirectory(QString dirPath) {
    if(dirPath.isEmpty()) {
        mEngineDir.clear();
        if(rescanTimer)
            rescanTimer->stop();
        EngineBridge::instance().stopWatch();
        return false;
    }
    if(!directoryUsable(dirPath))
        return false;

    // The listing is asynchronous — it arrives via onEngineDirListed(), and the
    // vectors keep serving the previous directory until then. A *refused* request
    // is not asynchronous though: nothing will ever arrive, so it has to fail
    // here rather than leave the caller staring at a silently empty folder.
    if(!EngineBridge::instance().openDir(dirPath, engineSortMode(mSortingMode),
                                         settings->showHiddenFiles())) {
        qWarning() << "[DirectoryManager] engine refused" << dirPath
                   << (qimgv_last_error() ? qimgv_last_error() : "");
        return false;
    }
    mDirectoryPath = dirPath;
    mEngineDir = dirPath;
    mAppliedSort = mSortingMode;
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
    // the parent's folder order, read synchronously: this answers a keypress
    DirectoryManager lookup;
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

int DirectoryManager::indexOfFile(QString filePath) const {
    auto item = find_if(fileEntryVec.begin(), fileEntryVec.end(), [filePath](const FSEntry& e) {
        return e.path == filePath;
    });
    if(item != fileEntryVec.end())
        return distance(fileEntryVec.begin(), item);
    return -1;
}

int DirectoryManager::indexOfDir(QString dirPath) const {
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

QString DirectoryManager::prevOfFile(QString filePath) const {
    QString prevFilePath = "";
    int currentIndex = indexOfFile(filePath);
    if(currentIndex > 0)
        prevFilePath = fileEntryVec.at(currentIndex - 1).path;
    return prevFilePath;
}

QString DirectoryManager::nextOfFile(QString filePath) const {
    QString nextFilePath = "";
    int currentIndex = indexOfFile(filePath);
    if(currentIndex >= 0 && currentIndex < static_cast<int>(fileEntryVec.size()) - 1)
        nextFilePath = fileEntryVec.at(currentIndex + 1).path;
    return nextFilePath;
}

QString DirectoryManager::prevOfDir(QString dirPath) const {
    QString prevDirectoryPath = "";
    int currentIndex = indexOfDir(dirPath);
    if(currentIndex > 0)
        prevDirectoryPath = dirEntryVec.at(currentIndex - 1).path;
    return prevDirectoryPath;
}

QString DirectoryManager::nextOfDir(QString dirPath) const {
    QString nextDirectoryPath = "";
    int currentIndex = indexOfDir(dirPath);
    if(currentIndex >= 0 && currentIndex < static_cast<int>(dirEntryVec.size()) - 1)
        nextDirectoryPath = dirEntryVec.at(currentIndex + 1).path;
    return nextDirectoryPath;
}

bool DirectoryManager::checkFileRange(int index) const {
    return index >= 0 && index < (int)fileEntryVec.size();
}

bool DirectoryManager::checkDirRange(int index) const {
    return index >= 0 && index < (int)dirEntryVec.size();
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

QDateTime DirectoryManager::lastModified(QString filePath) const {
    QFileInfo info;
    if(containsFile(filePath))
        info.setFile(filePath);
    return info.lastModified();
}

// TODO: what about symlinks?
inline
bool DirectoryManager::isSupportedFile(QString path) const {
    return ( isFile(path) && regex.match(path).hasMatch() );
}

bool DirectoryManager::isFile(QString path) const {
    if(!std::filesystem::exists(toStdString(path)))
        return false;
    if(!std::filesystem::is_regular_file(toStdString(path)))
        return false;
    return true;
}

bool DirectoryManager::isDir(QString path) const {
    if(!std::filesystem::exists(toStdString(path)))
        return false;
    if(!std::filesystem::is_directory(toStdString(path)))
        return false;
    return true;
}

bool DirectoryManager::isEmpty() const {
    return fileEntryVec.empty();
}

bool DirectoryManager::containsFile(QString filePath) const {
    return (std::find(fileEntryVec.begin(), fileEntryVec.end(), filePath) != fileEntryVec.end());
}

bool DirectoryManager::containsDir(QString dirPath) const {
    return (std::find(dirEntryVec.begin(), dirEntryVec.end(), dirPath) != dirEntryVec.end());
}

// ##############################################################
// ###################### PRIVATE METHODS #######################
// ##############################################################
// both directories & files
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
    std::sort(fileEntryVec.begin(), fileEntryVec.end(), std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
}

void DirectoryManager::setSortingMode(SortingMode mode) {
    if(mode != mSortingMode) {
        mSortingMode = mode;
        if(!mEngineDir.isEmpty()) {
            // engine resorts and reports back via onEngineDirListed()
            EngineBridge::instance().setSort(engineSortMode(mode));
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

// Entry management

// skips filename regex check
bool DirectoryManager::forceInsertFileEntry(const QString &filePath) {
    if(!this->isFile(filePath) || containsFile(filePath))
        return false;
    std::filesystem::directory_entry stdEntry(toStdString(filePath));
    QString fileName = QString::fromStdString(stdEntry.path().filename().generic_string()); // isn't it beautiful
    FSEntry FSEntry(filePath, fileName, stdEntry.file_size(), stdEntry.last_write_time(), stdEntry.is_directory());
    insert_sorted(fileEntryVec, FSEntry, std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
    if(!directoryPath().isEmpty()) {
        qCDebug(lcWatchPerf) << "fileIns" << filePath << directoryPath();
        emit fileAdded(filePath);
        // keep the engine listing in sync (the filter doesn't match this file)
        EngineBridge::instance().includePath(filePath);
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

void DirectoryManager::updateFileEntry(const QString &filePath) {
    if(!containsFile(filePath))
        return;
    FSEntry newEntry(filePath);
    int index = indexOfFile(filePath);
    if(fileEntryVec.at(index).modifyTime != newEntry.modifyTime)
        fileEntryVec.at(index) = newEntry;
    qCDebug(lcWatchPerf) << "fileMod" << filePath;
    emit fileModified(filePath);
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
                EngineBridge::instance().openDir(mDirectoryPath, engineSortMode(mSortingMode),
                                                 settings->showHiddenFiles());
        });
    }
    rescanTimer->start();
}

void DirectoryManager::onEngineDirListed(QString dirPath) {
    if(mResyncing || dirPath != mEngineDir)
        return;
    if(mDirectoryPath != dirPath)
        return;
    mResyncing = true;
    mirrorEngineListing(dirPath);
    mResyncing = false;
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

void DirectoryManager::onEngineMutated(int kind, QString path, bool isDir, quint64 size, qint64 mtimeMs) {
    if(mResyncing)
        return;
    switch(kind) {
        case QIMGV_MUT_ADDED: {
            if(containsFile(path) || containsDir(path))
                return; // app-side mutation already applied this
            FSEntry entry;
            entry.path = path;
            entry.name = QFileInfo(path).fileName();
            entry.isDirectory = isDir;
            entry.size = size;
            if(isDir)
                entry.modifyTime = fileTimeFromMs(0);
            else
                entry.modifyTime = fileTimeFromMs(mtimeMs);
            if(isDir) {
                insert_sorted(dirEntryVec, entry, std::bind(dirComparator(), this, std::placeholders::_1, std::placeholders::_2));
                emit dirAdded(path);
            } else {
                insert_sorted(fileEntryVec, entry, std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
                emit fileAdded(path);
            }
            break;
        }
        case QIMGV_MUT_MODIFIED:
            updateFileEntry(path);
            break;
        case QIMGV_MUT_REMOVED:
            removeDirEntry(path);
            removeFileEntry(path);
            break;
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

void DirectoryManager::onEngineOpenFailed(QString dirPath, int code) {
    if(dirPath != mEngineDir)
        return;
    qWarning() << "[DirectoryManager] engine open failed:" << dirPath << "code" << code;
    // an open that failed will never produce the listing its actions wait for
    pendingOnListed.erase(std::remove_if(pendingOnListed.begin(), pendingOnListed.end(),
                             [&](const PendingListed &p) { return p.dir == dirPath; }),
                          pendingOnListed.end());
    emit loadingFailed(dirPath);
}

void DirectoryManager::mirrorEngineListing(const QString &dirPath) {
    QElapsedTimer rebuildTimer;
    rebuildTimer.start();
    auto &bridge = EngineBridge::instance();

    // bulk pull: the whole listing crosses in one engine lock pass
    QByteArray listing;
    quint32 newFileCount = 0, newDirCount = 0;
    if(!bridge.exportListing(listing, newFileCount, newDirCount))
        return;

    // parse the arena into mirror entries
    QElapsedTimer parseTimer;
    parseTimer.start();
    std::vector<EngineBridge::EngineEntry> files, dirs;
    if(!EngineBridge::parseListing(listing, files, dirs))
        qWarning() << "[DirectoryManager] listing arena not consumed exactly:" << files.size()
                   << "files" << dirs.size() << "dirs from" << listing.size() << "bytes (counts"
                   << newFileCount << "+" << newDirCount << ")";
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

    // The engine snapshot may predate local mutations (operation-applied
    // creates/renames). Never regress to a stale listing: same-directory
    // entries that still exist on disk are kept and a rescan converges the
    // engine afterwards.
    std::vector<FSEntry> keepFiles, keepDirs;
    for(const auto &e : fileEntryVec)
        if(!newPaths.contains(e.path) && QFileInfo(e.path).absolutePath() == dirPath
           && QFileInfo(e.path).isFile())
            keepFiles.push_back(e);
    for(const auto &e : dirEntryVec)
        if(!newDirPaths.contains(e.path) && QFileInfo(e.path).absolutePath() == dirPath
           && QFileInfo(e.path).isDir())
            keepDirs.push_back(e);
    const bool engineBehind = !keepFiles.empty() || !keepDirs.empty();

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
        for(auto &e : keepFiles)
            fileEntryVec.push_back(std::move(e));
        for(auto &e : keepDirs)
            dirEntryVec.push_back(std::move(e));
        mPrevFileMeta = std::move(newMeta);
        mMirroredDir = dirPath;
        qDebug() << "[DirectoryManager] listing vectors rebuilt" << rebuildTimer2.elapsed() << "ms";
        // NOTE: emit loaded triggers the view-layer repopulation (100k items
        // measured ~400ms in the gui/ layer) — on the same UI thread but
        // belonging to the view, not to this mirror
        emit loaded(mDirectoryPath);
        qDebug() << "[DirectoryManager] listing rebuild (incl. view repopulation)"
                 << newFileCount << "files took" << rebuildTimer.elapsed() << "ms";
        if(engineBehind)
            scheduleRescan();
        return;
    }

    // same directory: granular resync — keep views/selection stable
    QSet<QString> oldPaths;
    for(const auto &f : fileEntryVec)
        oldPaths.insert(f.path);
    QSet<QString> oldDirPaths;
    for(const auto &d : dirEntryVec)
        oldDirPaths.insert(d.path);

    // 1. removals (descending index keeps earlier indexes valid); a missing
    //    entry that still exists on disk means a stale engine snapshot
    bool staleSnapshot = false;
    for(int i = static_cast<int>(fileEntryVec.size()) - 1; i >= 0; --i) {
        const QString &p = fileEntryVec[static_cast<size_t>(i)].path;
        if(!newPaths.contains(p)) {
            if(QFileInfo(p).isFile()) {
                staleSnapshot = true;
                continue;
            }
            emit fileRemoved(p, i);
            fileEntryVec.erase(fileEntryVec.begin() + i);
        }
    }
    for(int i = static_cast<int>(dirEntryVec.size()) - 1; i >= 0; --i) {
        const QString &p = dirEntryVec[static_cast<size_t>(i)].path;
        if(!newDirPaths.contains(p)) {
            if(QFileInfo(p).isDir()) {
                staleSnapshot = true;
                continue;
            }
            emit dirRemoved(p, i);
            dirEntryVec.erase(dirEntryVec.begin() + i);
        }
    }

    // 2. changed files: compare engine metadata against the previous mirror
    for(const auto &f : files) {
        if(oldPaths.contains(f.path)) {
            const auto prev = mPrevFileMeta.constFind(f.path);
            if(prev == mPrevFileMeta.constEnd() || prev->first != f.size || prev->second != f.mtimeMs)
                updateFileEntry(f.path); // re-stats natively, emits fileModified
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
    if(staleSnapshot || engineBehind)
        scheduleRescan();
}
