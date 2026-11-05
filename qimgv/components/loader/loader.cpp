#include "loader.h"

Loader::Loader() {
    pool = new QThreadPool(this);
    pool->setMaxThreadCount(2);
}

Loader::~Loader() {
    // same deal as the scaler's runnable: autoDelete(false) makes every task in
    // `tasks` ours to free. A finished task is normally taken by
    // onLoadFinished, but that needs the event loop — a task whose result never
    // got delivered is still here, and so is everything still queued.
    pool->waitForDone();
    qDeleteAll(tasks);
    tasks.clear();
}

void Loader::clearTasks() {
    clearPool();
    pool->waitForDone();
}

bool Loader::isBusy() const {
    return (tasks.count() != 0);
}

bool Loader::isLoading(const QString &path) {
    return tasks.contains(path);
}

std::optional<std::shared_ptr<Image>> Loader::load(const QString &path, const LoadPolicy &policy) {
    // sync path (editing fallback): always full quality
    return ImageFactory::createImage(path, false, policy);
}

// clears all buffered tasks before loading
void Loader::loadAsyncPriority(const QString &path, const LoadPolicy &policy) {
    clearPool();
    doLoadAsync(path, 1, true, policy); // tier-1 scaled for instant display
}

void Loader::loadAsync(const QString &path, const LoadPolicy &policy) {
    doLoadAsync(path, 0, true, policy);
}

void Loader::loadAsyncFull(const QString &path, const LoadPolicy &policy) {
    doLoadAsync(path, 0, false, policy); // tier-2 full-res upgrade
}

void Loader::doLoadAsync(const QString &path, int priority, bool scaled, const LoadPolicy &policy) {
    if(tasks.contains(path)) {
        return;
    }

    auto runnable = new LoaderRunnable(path, scaled, policy);
    runnable->setAutoDelete(false);
    tasks.insert(path, runnable);
    connect(runnable, &LoaderRunnable::finished, this, &Loader::onLoadFinished, Qt::UniqueConnection);
    pool->start(runnable, priority);
}

void Loader::onLoadFinished(std::optional<std::shared_ptr<Image>> image, const QString &path, bool scaled) {
    auto task = tasks.take(path);
    delete task;
    // this is where absence stops travelling: loadFinished() only ever fires
    // with an image, and the caller that cannot show one gets loadFailed()
    if(!image)
        emit loadFailed(path);
    else
        emit loadFinished(*image, path, scaled);
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
