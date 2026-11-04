#pragma once

#include <QThreadPool>
#include <optional>
#include "components/cache/thumbnailcache.h"
#include "loaderrunnable.h"

class Loader : public QObject {
    Q_OBJECT
public:
    explicit Loader();
    // nullopt when the file cannot be opened; the async path reports the same
    // case as loadFailed(), so loadFinished() never carries an absent image
    std::optional<std::shared_ptr<Image>> load(const QString &path, const LoadPolicy &policy);
    // tier-1: decode at screen resolution for instant display
    void loadAsyncPriority(const QString &path, const LoadPolicy &policy);
    void loadAsync(const QString &path, const LoadPolicy &policy);
    // tier-2: full-resolution re-decode (upgrade)
    void loadAsyncFull(const QString &path, const LoadPolicy &policy);

    void clearTasks();
    bool isBusy() const;
    bool isLoading(const QString &path);
private:
    QHash<QString, LoaderRunnable*> tasks;
    QThreadPool *pool;    
    void clearPool();
    void doLoadAsync(const QString &path, int priority, bool scaled, const LoadPolicy &policy);

signals:
    void loadFinished(std::shared_ptr<Image>, const QString &path, bool scaled);
    void loadFailed(const QString &path);

private slots:
    void onLoadFinished(std::optional<std::shared_ptr<Image>>, const QString&, bool scaled);
};
