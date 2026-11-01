#include "loader.h"

Loader::Loader() {
    pool = new QThreadPool(this);
    pool->setMaxThreadCount(2);
}

void Loader::clearTasks() {
    clearPool();
    pool->waitForDone();
}

bool Loader::isBusy() const {
    return (tasks.count() != 0);
}

bool Loader::isLoading(QString path) {
    return tasks.contains(path);
}

std::shared_ptr<Image> Loader::load(QString path) {
    // sync path (editing fallback): always full quality
    return ImageFactory::createImage(path, false);
}

// clears all buffered tasks before loading
void Loader::loadAsyncPriority(QString path) {
    clearPool();
    doLoadAsync(path, 1, true); // tier-1 scaled for instant display
}

void Loader::loadAsync(QString path) {
    doLoadAsync(path, 0, true);
}

void Loader::loadAsyncFull(QString path) {
    doLoadAsync(path, 0, false); // tier-2 full-res upgrade
}

void Loader::doLoadAsync(QString path, int priority, bool scaled) {
    if(tasks.contains(path)) {
        return;
    }

    auto runnable = new LoaderRunnable(path, scaled);
    runnable->setAutoDelete(false);
    tasks.insert(path, runnable);
    connect(runnable, &LoaderRunnable::finished, this, &Loader::onLoadFinished, Qt::UniqueConnection);
    pool->start(runnable, priority);
}

void Loader::onLoadFinished(std::shared_ptr<Image> image, const QString &path, bool scaled) {
    auto task = tasks.take(path);
    delete task;
    if(!image)
        emit loadFailed(path);
    else
        emit loadFinished(image, path, scaled);
}

void Loader::clearPool() {
    QHashIterator<QString, LoaderRunnable*> i(tasks);
    while (i.hasNext()) {
        i.next();
        if(pool->tryTake(i.value())) {
            delete tasks.take(i.key());
        }
    }
}
