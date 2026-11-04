#include "directorymodel.h"

DirectoryModel::DirectoryModel(QObject *parent) :
    QObject(parent)
{
    scaler = new Scaler(&cache);

    connect(&dirManager, &DirectoryManager::fileRemoved,  this, &DirectoryModel::onFileRemoved);
    connect(&dirManager, &DirectoryManager::fileAdded,    this, &DirectoryModel::onFileAdded);
    connect(&dirManager, &DirectoryManager::fileRenamed,  this, &DirectoryModel::onFileRenamed);
    connect(&dirManager, &DirectoryManager::fileModified, this, &DirectoryModel::onFileModified);
    connect(&dirManager, &DirectoryManager::dirRemoved,  this, &DirectoryModel::dirRemoved);
    connect(&dirManager, &DirectoryManager::dirAdded,    this, &DirectoryModel::dirAdded);
    connect(&dirManager, &DirectoryManager::dirRenamed,  this, &DirectoryModel::dirRenamed);

    connect(&dirManager, &DirectoryManager::loaded, this, &DirectoryModel::loaded);
    connect(&dirManager, &DirectoryManager::sortingChanged, this, &DirectoryModel::onSortingChanged);
    connect(&loader, &Loader::loadFinished, this, &DirectoryModel::onImageReady);
    connect(&loader, &Loader::loadFailed, this, &DirectoryModel::loadFailed);
}

DirectoryModel::~DirectoryModel() {
    loader.clearTasks();
    delete scaler;
}

int DirectoryModel::totalCount() const {
    return dirManager.totalCount();
}

int DirectoryModel::fileCount() const {
    return dirManager.fileCount();
}

int DirectoryModel::dirCount() const {
    return dirManager.dirCount();
}

int DirectoryModel::indexOfFile(const QString &filePath) const {
    return dirManager.indexOfFile(filePath);
}

int DirectoryModel::indexOfDir(const QString &filePath) const {
    return dirManager.indexOfDir(filePath);
}

SortingMode DirectoryModel::sortingMode() const {
    return dirManager.sortingMode();
}

const FSEntry &DirectoryModel::fileEntryAt(int index) const {
    return dirManager.fileEntryAt(index);
}

QString DirectoryModel::fileNameAt(int index) const {
    return dirManager.fileNameAt(index);
}

QString DirectoryModel::filePathAt(int index) const {
    return dirManager.filePathAt(index);
}

QString DirectoryModel::dirNameAt(int index) const {
    return dirManager.dirNameAt(index);
}

QString DirectoryModel::dirPathAt(int index) const {
    return dirManager.dirPathAt(index);
}



QString DirectoryModel::directoryPath() const {
    return dirManager.directoryPath();
}

bool DirectoryModel::containsFile(const QString &filePath) const {
    return dirManager.containsFile(filePath);
}

bool DirectoryModel::containsDir(const QString &dirPath) const {
    return dirManager.containsDir(dirPath);
}

bool DirectoryModel::isEmpty() const {
    return dirManager.isEmpty();
}

QString DirectoryModel::firstFile() const {
    return dirManager.firstFile();
}

QString DirectoryModel::lastFile() const {
    return dirManager.lastFile();
}

QString DirectoryModel::nextOf(const QString &filePath) const {
    return dirManager.nextOfFile(filePath);
}

QString DirectoryModel::prevOf(const QString &filePath) const {
    return dirManager.prevOfFile(filePath);
}

QDateTime DirectoryModel::lastModified(const QString &filePath) const {
    return dirManager.lastModified(filePath);
}

// -----------------------------------------------------------------------------

bool DirectoryModel::forceInsert(const QString &filePath) {
    return dirManager.forceInsertFileEntry(filePath);
}

void DirectoryModel::setSortingMode(SortingMode mode) {
    dirManager.setSortingMode(mode);
}

void DirectoryModel::removeFile(const QString &filePath, bool trash, FileOpResult &result) {
    // no mirror write here: the engine's watch sees the change and the
    // listing follows via fileRemoved — one writer, no reconciliation
    if(trash)
        FileOperations::moveToTrash(filePath, result);
    else
        FileOperations::removeFile(filePath, result);
}

void DirectoryModel::renameEntry(const QString &oldPath, const QString &newName, bool force, FileOpResult &result) {
    // the engine watch surfaces a rename as remove+add — an in-app rename now
    // rides the exact path an external rename takes
    FileOperations::rename(oldPath, newName, force, result);
}

void DirectoryModel::removeDir(const QString &dirPath, bool trash, bool recursive, FileOpResult &result) {
    if(trash) {
        FileOperations::moveToTrash(dirPath, result);
    } else {
        FileOperations::removeDir(dirPath, recursive, result);
    }
    if(result != FileOpResult::SUCCESS)
        return;
}

void DirectoryModel::copyFileTo(const QString &srcFile, const QString &destDirPath, bool force, FileOpResult &result) {
    FileOperations::copyFileTo(srcFile, destDirPath, force, result);
}

void DirectoryModel::moveFileTo(const QString &srcFile, const QString &destDirPath, bool force, FileOpResult &result) {
    // moving out of the watched directory surfaces as a removal event; the
    // old processEvents() ordering hack died with the local write path
    FileOperations::moveFileTo(srcFile, destDirPath, force, result);
}
// -----------------------------------------------------------------------------
bool DirectoryModel::setDirectory(const QString &path) {
    cache.clear();
    return dirManager.setDirectory(path);
}

qint64 DirectoryModel::whenListed(const QString &dirPath, std::function<void()> action) {
    return dirManager.whenListed(dirPath, std::move(action));
}

bool DirectoryModel::cancelListed(qint64 token) {
    return dirManager.cancelListed(token);
}

void DirectoryModel::unload(int index) {
    QString filePath = this->filePathAt(index);
    cache.remove(filePath);
}

void DirectoryModel::unload(const QString &filePath) {
    cache.remove(filePath);
}

void DirectoryModel::unloadExcept(const QString &filePath, bool keepNearby) {
    QList<QString> list;
    list << filePath;
    if(keepNearby)  {
        list << prevOf(filePath);
        list << nextOf(filePath);
    }
    cache.trimTo(list);
}

bool DirectoryModel::loaderBusy() const {
    return loader.isBusy();
}

bool DirectoryModel::listingSettled() const {
    return dirManager.isSettled();
}

void DirectoryModel::onImageReady(std::shared_ptr<Image> img, const QString &path, bool scaled) {
    if(!img) {
        emit loadFailed(path);
        return;
    }
    cache.remove(path);
    cache.insert(img);
    if(scaled)
        mScaledCache.insert(path);
    else
        mScaledCache.remove(path);
    emit imageReady(img, path, scaled);
}

bool DirectoryModel::saveFile(const QString &filePath) {
    return saveFile(filePath, filePath);
}

bool DirectoryModel::saveFile(const QString &filePath, const QString &destPath) {
    if(!containsFile(filePath) || !cache.contains(filePath))
        return false;
    auto img = cache.get(filePath);
    if(img->save(destPath)) {
        // listing changes (modified/new) surface through the engine's watch
        // and arrive via fileModified/fileAdded — same as any external edit
        return true;
    }
    return false;
}

// dirManager events

void DirectoryModel::onSortingChanged() {
    emit sortingChanged(sortingMode());
}

void DirectoryModel::onFileAdded(const QString &filePath) {
    emit fileAdded(filePath);
}

void DirectoryModel::onFileModified(const QString &filePath) {
    QDateTime modTime = lastModified(filePath);
    if(modTime.isValid()) {
        auto img = cache.get(filePath);
        if(img) {
            // check if file on disk is different
            if(modTime != img->lastModified())
                reload(filePath);
        }
        emit fileModified(filePath);
    }
}

void DirectoryModel::onFileRemoved(const QString &filePath, int index) {
    unload(filePath);
    emit fileRemoved(filePath, index);
}

void DirectoryModel::onFileRenamed(const QString &fromPath, int indexFrom, const QString &toPath, int indexTo) {
    unload(fromPath);
    emit fileRenamed(fromPath, indexFrom, toPath, indexTo);
}

bool DirectoryModel::isLoaded(int index) const {
    return cache.contains(filePathAt(index));
}

bool DirectoryModel::isLoaded(const QString &filePath) const {
    return cache.contains(filePath);
}

std::shared_ptr<Image> DirectoryModel::getImageAt(int index) {
    return getImage(filePathAt(index));
}

// returns cached image
// if image is not cached, loads it in the main thread
// for async access use loadAsync(), then catch onImageReady()
std::shared_ptr<Image> DirectoryModel::getImage(const QString &filePath) {
    std::shared_ptr<Image> img = cache.get(filePath);
    if(img)
        return img;
    // the one place absence is flattened back into a null pointer, because the
    // callers of getImage() hand it straight to sinks that test for it
    // (guiSetImage, getMimeDataForImage). Everything upstream of this line
    // carries std::optional and cannot forward the question unknowingly.
    return loader.load(filePath).value_or(nullptr);
}

void DirectoryModel::updateImage(const QString &filePath, std::shared_ptr<Image> img) {
    if(containsFile(filePath) /*& cache.contains(filePath)*/) {
        if(!cache.contains(filePath)) {
            cache.insert(img);
        } else {
            cache.insert(img);
            emit imageUpdated(filePath);
        }
    }
}

void DirectoryModel::load(const QString &filePath, bool asyncHint) {
    load(filePath, asyncHint, true); // tier-1 by default
}

void DirectoryModel::load(const QString &filePath, bool asyncHint, bool scaled) {
    if(!containsFile(filePath) || loader.isLoading(filePath))
        return;
    if(!cache.contains(filePath)) {
        if(asyncHint) {
            loader.loadAsyncPriority(filePath);
        } else {
            // sync path (editing): full quality
            auto img = loader.load(filePath);
            if(img) {
                cache.insert(*img);
                emit imageReady(*img, filePath, false);
            } else {
                emit loadFailed(filePath);
            }
        }
    } else {
        bool cachedScaled = mScaledCache.contains(filePath);
        // instant display from cache; a tier-1 cached image upgrades to full
        if(cachedScaled && scaled)
            loader.loadAsyncFull(filePath);
        emit imageReady(cache.get(filePath), filePath, cachedScaled);
    }
}

void DirectoryModel::reload(const QString &filePath) {
    if(cache.contains(filePath)) {
        cache.remove(filePath);
        load(filePath, false);
    }
}

void DirectoryModel::preload(const QString &filePath) {
    if(containsFile(filePath) && !cache.contains(filePath))
        loader.loadAsync(filePath); // tier-1 scaled
}

// tier-2: full-res re-decode of a tier-1 cached image
void DirectoryModel::upgradeToFull(const QString &filePath) {
    if(containsFile(filePath) && !loader.isLoading(filePath) && mScaledCache.contains(filePath))
        loader.loadAsyncFull(filePath);
}
