#pragma once

#include <QThreadPool>
#include "components/cache/thumbnailcache.h"
#include "loaderrunnable.h"

class Loader : public QObject {
    Q_OBJECT
public:
    explicit Loader();
    std::shared_ptr<Image> load(QString path);
    // tier-1: decode at screen resolution for instant display
    void loadAsyncPriority(QString path);
    void loadAsync(QString path);
    // tier-2: full-resolution re-decode (upgrade)
    void loadAsyncFull(QString path);

    void clearTasks();
    bool isBusy() const;
    bool isLoading(QString path);
private:
    QHash<QString, LoaderRunnable*> tasks;
    QThreadPool *pool;    
    void clearPool();
    void doLoadAsync(QString path, int priority, bool scaled);

signals:
    void loadFinished(std::shared_ptr<Image>, const QString &path, bool scaled);
    void loadFailed(const QString &path);

private slots:
    void onLoadFinished(std::shared_ptr<Image>, const QString&, bool scaled);
};
