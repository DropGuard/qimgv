#pragma once

#include <QObject>
#include "cache/cache.h"
#include "directorymanager/directorymanager.h"
#include "scaler/scaler.h"
#include "loader/loader.h"
#include "utils/fileoperations.h"

class DirectoryModel : public QObject {
    Q_OBJECT
public:
    explicit DirectoryModel(WorkerPool &pool, QObject *parent = nullptr);
    ~DirectoryModel() override;

    Scaler *scaler = nullptr;

    void load(const QString &filePath, bool asyncHint);
    void load(const QString &filePath, bool asyncHint, bool scaled);
    void preload(const QString &filePath);
    void upgradeToFull(const QString &filePath);

    int fileCount() const;
    int dirCount() const;
    int indexOfFile(const QString &filePath) const;
    int indexOfDir(const QString &filePath) const;
    QString fileNameAt(int index) const;
    bool containsFile(const QString &filePath) const;
    bool isEmpty() const;
    QString nextOf(const QString &filePath) const;
    QString prevOf(const QString &filePath) const;
    QString firstFile() const;
    QString lastFile() const;
    QDateTime lastModified(const QString &filePath) const;

    void copyFileTo(const QString &srcFile, const QString &destDirPath, bool force, FileOpResult &result);
    void moveFileTo(const QString &srcFile, const QString &destDirPath, bool force, FileOpResult &result);
    void renameEntry(const QString &oldFilePath, const QString &newName, bool force, FileOpResult &result);
    void removeFile(const QString &filePath, bool trash, FileOpResult &result);
    void removeDir(const QString &dirPath, bool trash, bool recursive, FileOpResult &result);

    bool setDirectory(const QString &);
    // deferred listing actions, straight through to the manager
    qint64 whenListed(const QString &dirPath, std::function<void()> action);
    bool cancelListed(qint64 token);

    void unload(int index);

    bool loaderBusy() const;
    // the manager's "no listing work in flight", forwarded the way loader state
    // already is: a caller waiting on the engine should not need the manager
    bool listingSettled() const;

    std::shared_ptr<Image> getImageAt(int index);
    std::shared_ptr<Image> getImage(const QString &filePath);

    void updateImage(const QString &filePath, std::shared_ptr<Image> img);

    void setSortingMode(SortingMode mode);
    SortingMode sortingMode() const;

    QString directoryPath() const;
    void unload(const QString &filePath);
    bool isLoaded(int index) const;
    bool isLoaded(const QString &filePath) const;
    void reload(const QString &filePath);
    QString filePathAt(int index) const;
    void unloadExcept(const QString &filePath, bool keepNearby);
    const FSEntry &fileEntryAt(int index) const;

    int totalCount() const;
    QString dirNameAt(int index) const;
    QString dirPathAt(int index) const;


    bool saveFile(const QString &filePath);
    bool saveFile(const QString &filePath, const QString &destPath);

    bool containsDir(const QString &dirPath) const;
signals:
    void fileRemoved(const QString &filePath, int index);
    void fileAdded(const QString &filePath);
    void fileModified(const QString &filePath);
    void dirRemoved(const QString &dirPath, int index);
    void dirAdded(const QString &dirPath);
    void loaded(const QString &filePath);
    void loadFailed(const QString &path);
    void sortingChanged(SortingMode);
    void imageReady(std::shared_ptr<Image> img, const QString&, bool scaled);
    void imageUpdated(const QString &filePath);

private:
    DirectoryManager dirManager;
    Loader loader;
    QSet<QString> mScaledCache; // paths whose cached image is tier-1 (scaled)
    Cache cache;

private slots:
    void onImageReady(std::shared_ptr<Image> img, const QString &path, bool scaled);
    void onSortingChanged();
    void onFileAdded(const QString &filePath);
    void onFileRemoved(const QString &filePath, int index);
    void onFileModified(const QString &filePath);
};
