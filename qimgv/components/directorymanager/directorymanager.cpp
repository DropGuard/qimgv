#include "directorymanager.h"

#include "enginebridge.h"
#include "qimgv_engine.h"

#include <QTimer>
#include <chrono>

namespace fs = std::filesystem;

DirectoryManager::DirectoryManager() :
    watcher(nullptr),
    mSortingMode(SORT_NAME)
{
    regex.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    collator.setNumericMode(true);

    readSettings();
    setSortingMode(settings->sortingMode());
    connect(settings, &Settings::settingsChanged, this, &DirectoryManager::readSettings);

    mEngineWatching = EngineBridge::supportsWatching();
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

void DirectoryManager::startFileWatcher(QString directoryPath) {
    if(directoryPath == "")
        return;
    if(!watcher)
        watcher = DirectoryWatcher::newInstance();

    connect(watcher, &DirectoryWatcher::fileCreated,  this, &DirectoryManager::onFileAddedExternal,    Qt::UniqueConnection);
    connect(watcher, &DirectoryWatcher::fileDeleted,  this, &DirectoryManager::onFileRemovedExternal,  Qt::UniqueConnection);
    connect(watcher, &DirectoryWatcher::fileModified, this, &DirectoryManager::onFileModifiedExternal, Qt::UniqueConnection);
    connect(watcher, &DirectoryWatcher::fileRenamed,  this, &DirectoryManager::onFileRenamedExternal,  Qt::UniqueConnection);

    watcher->setWatchPath(directoryPath);
    watcher->observe();
}

void DirectoryManager::stopFileWatcher() {
    if(!watcher)
        return;

    watcher->stopObserving();

    disconnect(watcher, &DirectoryWatcher::fileCreated,  this, &DirectoryManager::onFileAddedExternal);
    disconnect(watcher, &DirectoryWatcher::fileDeleted,  this, &DirectoryManager::onFileRemovedExternal);
    disconnect(watcher, &DirectoryWatcher::fileModified, this, &DirectoryManager::onFileModifiedExternal);
    disconnect(watcher, &DirectoryWatcher::fileRenamed,  this, &DirectoryManager::onFileRenamedExternal);
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
        if(mEngineWatching)
            EngineBridge::instance().stopWatch();
        return false;
    }
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
    mListSource = SOURCE_DIRECTORY;
    mDirectoryPath = dirPath;

    // async: the listing arrives via onEngineDirListed(); the vectors keep
    // serving the previous directory until then
    EngineBridge::instance().openDir(dirPath, engineSortMode(mSortingMode),
                                     settings->showHiddenFiles());
    mEngineDir = dirPath;
    mAppliedSort = mSortingMode;
    if (!mEngineWatching)
        startFileWatcher(dirPath); // engine watcher handles it on Linux
    return true;
}

bool DirectoryManager::setDirectoryRecursive(QString dirPath) {
    if(dirPath.isEmpty()) {
        return false;
    }
    if(!std::filesystem::exists(toStdString(dirPath))) {
        qDebug() << "[DirectoryManager] Error - path does not exist.";
        return false;
    }
    if(!std::filesystem::is_directory(toStdString(dirPath))) {
        qDebug() << "[DirectoryManager] Error - path is not a directory.";
        return false;
    }
    stopFileWatcher();
    mListSource = SOURCE_DIRECTORY_RECURSIVE;
    mEngineDir.clear();
    mMirroredDir.clear();
    mDirectoryPath = dirPath;
    loadEntryList(dirPath, true);
    sortEntryLists();
    emit loaded(dirPath);
    return true;
}

QString DirectoryManager::directoryPath() const {
    if(mListSource == SOURCE_DIRECTORY || mListSource == SOURCE_DIRECTORY_RECURSIVE)
        return mDirectoryPath;
    else
        return "";
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
    if(currentIndex >= 0 && currentIndex < fileEntryVec.size() - 1)
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
    if(currentIndex >= 0 && currentIndex < dirEntryVec.size() - 1)
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
void DirectoryManager::loadEntryList(QString directoryPath, bool recursive) {
    dirEntryVec.clear();
    fileEntryVec.clear();
    if(recursive) { // load files only
        addEntriesFromDirectoryRecursive(fileEntryVec, directoryPath);
    } else { // load dirs & files
        addEntriesFromDirectory(fileEntryVec, directoryPath);
    }
}

// both directories & files
void DirectoryManager::addEntriesFromDirectory(std::vector<FSEntry> &entryVec, QString directoryPath) {
    QRegularExpressionMatch match;
    for(const auto & entry : fs::directory_iterator(toStdString(directoryPath))) {
        QString name = QString::fromStdString(entry.path().filename().generic_string());
#ifndef Q_OS_WIN32
        // ignore hidden files
        if(!settings->showHiddenFiles() && name.startsWith("."))
            continue;
#else
        DWORD attributes = GetFileAttributes(entry.path().generic_string().c_str());
        if(!settings->showHiddenFiles() && attributes & FILE_ATTRIBUTE_HIDDEN)
            continue;
#endif
        QString path = QString::fromStdString(entry.path().generic_string());
        match = regex.match(name);
        if(entry.is_directory()) { // this can still throw std::bad_alloc ..
            FSEntry newEntry;
            try {
                newEntry.name = name;
                newEntry.path = path;
                newEntry.isDirectory = true;
                //newEntry.size = entry.file_size();
                //newEntry.modifyTime = entry.last_write_time();
            } catch (const std::filesystem::filesystem_error &err) {
                qDebug() << "[DirectoryManager]" << err.what();
                continue;
            }
            dirEntryVec.emplace_back(newEntry);
        } else if (match.hasMatch()) {
            FSEntry newEntry;
            try {
                newEntry.name = name;
                newEntry.path = path;
                newEntry.isDirectory = false;
                newEntry.size = entry.file_size();
                newEntry.modifyTime = entry.last_write_time();
            } catch (const std::filesystem::filesystem_error &err) {
                qDebug() << "[DirectoryManager]" << err.what();
                continue;
            }
            entryVec.emplace_back(newEntry);
        }
    }
}

void DirectoryManager::addEntriesFromDirectoryRecursive(std::vector<FSEntry> &entryVec, QString directoryPath) {
    QRegularExpressionMatch match;
    for(const auto & entry : fs::recursive_directory_iterator(toStdString(directoryPath))) {
        QString name = QString::fromStdString(entry.path().filename().generic_string());
        QString path = QString::fromStdString(entry.path().generic_string());
        match = regex.match(name);
        if(!entry.is_directory() && match.hasMatch()) {
            FSEntry newEntry;
            try {
                newEntry.name = name;
                newEntry.path = path;
                newEntry.isDirectory = false;
                newEntry.size = entry.file_size();
                newEntry.modifyTime = entry.last_write_time();
            } catch (const std::filesystem::filesystem_error &err) {
                qDebug() << "[DirectoryManager]" << err.what();
                continue;
            }
            entryVec.emplace_back(newEntry);
        }
    }
}

void DirectoryManager::sortEntryLists() {
    if(settings->sortFolders())
        std::sort(dirEntryVec.begin(), dirEntryVec.end(), std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
    else
        std::sort(dirEntryVec.begin(), dirEntryVec.end(), std::bind(&DirectoryManager::path_entry_compare, this, std::placeholders::_1, std::placeholders::_2));
    std::sort(fileEntryVec.begin(), fileEntryVec.end(), std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
}

void DirectoryManager::setSortingMode(SortingMode mode) {
    if(mode != mSortingMode) {
        mSortingMode = mode;
        if(mListSource == SOURCE_DIRECTORY && !mEngineDir.isEmpty()) {
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

bool DirectoryManager::insertFileEntry(const QString &filePath) {
    if(!isSupportedFile(filePath))
        return false;
    return forceInsertFileEntry(filePath);
}

// skips filename regex check
bool DirectoryManager::forceInsertFileEntry(const QString &filePath) {
    if(!this->isFile(filePath) || containsFile(filePath))
        return false;
    std::filesystem::directory_entry stdEntry(toStdString(filePath));
    QString fileName = QString::fromStdString(stdEntry.path().filename().generic_string()); // isn't it beautiful
    FSEntry FSEntry(filePath, fileName, stdEntry.file_size(), stdEntry.last_write_time(), stdEntry.is_directory());
    insert_sorted(fileEntryVec, FSEntry, std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
    if(!directoryPath().isEmpty()) {
        qDebug() << "fileIns" << filePath << directoryPath();
        emit fileAdded(filePath);
        // keep the engine listing in sync (the filter doesn't match this file)
        EngineBridge::instance().includePath(filePath);
    }
    if(!mEngineWatching)
        scheduleRescan();
    return true;
}

void DirectoryManager::removeFileEntry(const QString &filePath) {
    if(!containsFile(filePath))
        return;
    int index = indexOfFile(filePath);
    fileEntryVec.erase(fileEntryVec.begin() + index);
    qDebug() << "fileRem" << filePath;
    emit fileRemoved(filePath, index);
    if(!mEngineWatching)
        scheduleRescan();
}

void DirectoryManager::updateFileEntry(const QString &filePath) {
    if(!containsFile(filePath))
        return;
    FSEntry newEntry(filePath);
    int index = indexOfFile(filePath);
    if(fileEntryVec.at(index).modifyTime != newEntry.modifyTime)
        fileEntryVec.at(index) = newEntry;
    qDebug() << "fileMod" << filePath;
    emit fileModified(filePath);
    if(!mEngineWatching)
        scheduleRescan();
}

void DirectoryManager::renameFileEntry(const QString &oldFilePath, const QString &newFileName) {
    QFileInfo fi(oldFilePath);
    QString newFilePath = fi.absolutePath() + "/" + newFileName;
    if(!containsFile(oldFilePath)) {
        if(containsFile(newFilePath))
            updateFileEntry(newFilePath);
        else
            insertFileEntry(newFilePath);
        return;
    }
    if(!isSupportedFile(newFilePath)) {
        removeFileEntry(oldFilePath);
        return;
    }
    if(containsFile(newFilePath)) {
        int replaceIndex = indexOfFile(newFilePath);
        fileEntryVec.erase(fileEntryVec.begin() + replaceIndex);
        emit fileRemoved(newFilePath, replaceIndex);
    }
    // remove the old one
    int oldIndex = indexOfFile(oldFilePath);
    fileEntryVec.erase(fileEntryVec.begin() + oldIndex);
    // insert
    std::filesystem::directory_entry stdEntry(toStdString(newFilePath));
    FSEntry FSEntry(newFilePath, newFileName, stdEntry.file_size(), stdEntry.last_write_time(), stdEntry.is_directory());
    insert_sorted(fileEntryVec, FSEntry, std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
    qDebug() << "fileRen" << oldFilePath << newFilePath;
    emit fileRenamed(oldFilePath, oldIndex, newFilePath, indexOfFile(newFilePath));
    if(!mEngineWatching)
        scheduleRescan();
}

// ---- dir entries

bool DirectoryManager::insertDirEntry(const QString &dirPath) {
    if(containsDir(dirPath))
        return false;
    std::filesystem::directory_entry stdEntry(toStdString(dirPath));
    QString dirName = QString::fromStdString(stdEntry.path().filename().generic_string()); // isn't it beautiful
    FSEntry FSEntry;
    FSEntry.name = dirName;
    FSEntry.path = dirPath;
    FSEntry.isDirectory = true;
    insert_sorted(dirEntryVec, FSEntry, std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
    qDebug() << "dirIns" << dirPath;
    emit dirAdded(dirPath);
    if(!mEngineWatching)
        scheduleRescan();
    return true;
}

void DirectoryManager::removeDirEntry(const QString &dirPath) {
    if(!containsDir(dirPath))
        return;
    int index = indexOfDir(dirPath);
    dirEntryVec.erase(dirEntryVec.begin() + index);
    qDebug() << "dirRem" << dirPath;
    emit dirRemoved(dirPath, index);
    if(!mEngineWatching)
        scheduleRescan();
}

void DirectoryManager::renameDirEntry(const QString &oldDirPath, const QString &newDirName) {
    if(!containsDir(oldDirPath))
        return;
    QFileInfo fi(oldDirPath);
    QString newDirPath = fi.absolutePath() + "/" + newDirName;
    // remove the old one
    int oldIndex = indexOfDir(oldDirPath);
    dirEntryVec.erase(dirEntryVec.begin() + oldIndex);
    // insert
    std::filesystem::directory_entry stdEntry(toStdString(newDirPath));
    FSEntry FSEntry;
    FSEntry.name = newDirName;
    FSEntry.path = newDirPath;
    FSEntry.isDirectory = true;
    insert_sorted(dirEntryVec, FSEntry, std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
    qDebug() << "dirRen" << oldDirPath << newDirPath;
    emit dirRenamed(oldDirPath, oldIndex, newDirPath, indexOfDir(newDirPath));
    if(!mEngineWatching)
        scheduleRescan();
}


FileListSource DirectoryManager::source() const {
    return mListSource;
}

QStringList DirectoryManager::fileList() const {
    QStringList list;
    for(auto const& value : fileEntryVec)
        list << value.path;
    return list;
}

bool DirectoryManager::fileWatcherActive() {
    if(!watcher)
        return false;
    return watcher->isObserving();
}

//----------------------------------------------------------------------------
// fs watcher events  ( onFile___External() )
// these take file NAMES, not paths
void DirectoryManager::onFileRemovedExternal(QString fileName) {
    QString fullPath = watcher->watchPath() + "/" + fileName;
    removeDirEntry(fullPath);
    removeFileEntry(fullPath);
}

void DirectoryManager::onFileAddedExternal(QString fileName) {
    QString fullPath = watcher->watchPath() + "/" + fileName;
    if(isDir(fullPath))
        insertDirEntry(fullPath);
    else
        insertFileEntry(fullPath);
}

void DirectoryManager::onFileRenamedExternal(QString oldName, QString newName) {
    QString oldPath = watcher->watchPath() + "/" + oldName;
    QString newPath = watcher->watchPath() + "/" + newName;
    if(isDir(newPath))
        renameDirEntry(oldPath, newName);
    else
        renameFileEntry(oldPath, newName);
}

void DirectoryManager::onFileModifiedExternal(QString fileName) {
    updateFileEntry(watcher->watchPath() + "/" + fileName);
}

//----------------------------------------------------------------------------
// rust engine bridge

// settings.h's SortingMode order differs from the QIMGV_SORT_* codes — map
// explicitly, never positionally.
int DirectoryManager::engineSortMode(SortingMode mode) {
    switch(mode) {
        case SORT_NAME:       return 0;
        case SORT_NAME_DESC:  return 1;
        case SORT_SIZE:       return 4;
        case SORT_SIZE_DESC:  return 5;
        case SORT_TIME:       return 2;
        case SORT_TIME_DESC:  return 3;
    }
    return 0;
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

// watcher events and in-app file operations both land here; the vectors get
// the instant update, the engine rescan reconciles a beat later (debounced).
void DirectoryManager::scheduleRescan() {
    if(mResyncing)
        return;
    if(mListSource != SOURCE_DIRECTORY || mDirectoryPath.isEmpty())
        return;
    if(!rescanTimer) {
        rescanTimer = new QTimer(this);
        rescanTimer->setSingleShot(true);
        rescanTimer->setInterval(150);
        connect(rescanTimer, &QTimer::timeout, this, [this]() {
            if(!mResyncing && mListSource == SOURCE_DIRECTORY && !mDirectoryPath.isEmpty())
                EngineBridge::instance().openDir(mDirectoryPath, engineSortMode(mSortingMode),
                                                 settings->showHiddenFiles());
        });
    }
    rescanTimer->start();
}

void DirectoryManager::onEngineDirListed(QString dirPath) {
    if(mResyncing || dirPath != mEngineDir || mListSource != SOURCE_DIRECTORY)
        return;
    if(mDirectoryPath != dirPath)
        return;
    mResyncing = true;
    mirrorEngineListing(dirPath);
    mResyncing = false;
}

void DirectoryManager::onEngineMutated(int kind, QString path, bool isDir, quint64 size, qint64 mtimeMs) {
    if(mListSource != SOURCE_DIRECTORY || mResyncing)
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
                insert_sorted(dirEntryVec, entry, std::bind(compareFunction(), this, std::placeholders::_1, std::placeholders::_2));
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

void DirectoryManager::onEngineOpenFailed(QString dirPath, int code) {
    if(dirPath != mEngineDir || mListSource != SOURCE_DIRECTORY)
        return;
    qWarning() << "[DirectoryManager] engine open failed:" << dirPath << "code" << code;
    emit loadingFailed(dirPath);
}

void DirectoryManager::mirrorEngineListing(const QString &dirPath) {
    QElapsedTimer rebuildTimer;
    rebuildTimer.start();
    auto &bridge = EngineBridge::instance();
    const int newFileCount = bridge.fileCount();
    const int newDirCount = bridge.dirCount();
    qDebug() << "[DirectoryManager] mirrored listing" << dirPath
             << "files:" << newFileCount << "dirs:" << newDirCount;

    // snapshot the engine listing
    std::vector<EngineBridge::EngineEntry> files, dirs;
    files.reserve(newFileCount);
    dirs.reserve(newDirCount);
    EngineBridge::EngineEntry e;
    for(int i = 0; i < newFileCount; ++i)
        if(bridge.entryAt(i, false, e))
            files.push_back(e);
    for(int i = 0; i < newDirCount; ++i)
        if(bridge.entryAt(i, true, e))
            dirs.push_back(e);

    QHash<QString, QPair<quint64, qint64>> newMeta;
    for(const auto &f : files)
        newMeta.insert(f.path, {f.size, f.mtimeMs});

    QSet<QString> newPaths;
    for(const auto &f : files)
        newPaths.insert(f.path);
    QSet<QString> newDirPaths;
    for(const auto &d : dirs)
        newDirPaths.insert(d.path);

    // The engine snapshot may predate local mutations (watcher-applied
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
        emit loaded(mDirectoryPath);
        qDebug() << "[DirectoryManager] listing rebuild" << newFileCount << "files took" << rebuildTimer.elapsed() << "ms";
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
