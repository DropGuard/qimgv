#include "directorypresenter.h"

#include <utility>

DirectoryPresenter::DirectoryPresenter(WorkerPool &_pool, QObject *parent) :
    QObject(parent),
    pool(_pool),
    thumbnailer(_pool, EngineBridge::instance()),
    mShowDirs(false) {
    connect(&thumbnailer, &Thumbnailer::thumbnailReady, this, &DirectoryPresenter::onThumbnailReady);
}

void DirectoryPresenter::unsetModel() {
    disconnect(model.get(), &DirectoryModel::fileRemoved,  this, &DirectoryPresenter::onFileRemoved);
    disconnect(model.get(), &DirectoryModel::fileAdded,    this, &DirectoryPresenter::onFileAdded);
    disconnect(model.get(), &DirectoryModel::fileModified, this, &DirectoryPresenter::onFileModified);
    disconnect(model.get(), &DirectoryModel::dirRemoved,   this, &DirectoryPresenter::onDirRemoved);
    disconnect(model.get(), &DirectoryModel::dirAdded,     this, &DirectoryPresenter::onDirAdded);
    model = nullptr;
    // also empty view?
}

void DirectoryPresenter::setModel(std::shared_ptr<DirectoryModel> newModel) {
    if(model)
        unsetModel();
    if(!newModel)
        return;
    model = newModel;
    populateView();

    // filesystem changes
    connect(model.get(), &DirectoryModel::fileRemoved,  this, &DirectoryPresenter::onFileRemoved);
    connect(model.get(), &DirectoryModel::fileAdded,    this, &DirectoryPresenter::onFileAdded);
    connect(model.get(), &DirectoryModel::fileModified, this, &DirectoryPresenter::onFileModified);
    connect(model.get(), &DirectoryModel::dirRemoved,   this, &DirectoryPresenter::onDirRemoved);
    connect(model.get(), &DirectoryModel::dirAdded,     this, &DirectoryPresenter::onDirAdded);
}

void DirectoryPresenter::reloadModel() {
    populateView();
}

void DirectoryPresenter::populateView() {
    if(!model || !view)
        return;
    view->populate(mShowDirs ? model->totalCount() : model->fileCount());
    selectAndFocus(0);
}

void DirectoryPresenter::disconnectView() {
    if(!view)
        return;
    // the pairs come from setView's own list, so this cannot drift from it
    if(viewTeardown)
        viewTeardown();
    viewTeardown = nullptr;
    viewSender = nullptr;
    view = nullptr;
}

//------------------------------------------------------------------------------

void DirectoryPresenter::onFileRemoved(const QString &filePath, int index) {
    Q_UNUSED(filePath)
    if(!view)
        return;
    view->removeItem(mShowDirs ? index + model->dirCount() : index);
}

void DirectoryPresenter::onFileAdded(const QString &filePath) {
    if(!view)
        return;
    int index = model->indexOfFile(filePath);
    view->insertItem(mShowDirs ? model->dirCount() + index : index);
}

void DirectoryPresenter::onFileModified(const QString &filePath) {
    if(!view)
        return;
    int index = model->indexOfFile(filePath);
    view->reloadItem(mShowDirs ? model->dirCount() + index : index);
}

void DirectoryPresenter::onDirRemoved(const QString &dirPath, int index) {
    Q_UNUSED(dirPath)
    if(!view || !mShowDirs)
        return;
    view->removeItem(index);
}

void DirectoryPresenter::onDirAdded(const QString &dirPath) {
    if(!view || !mShowDirs)
        return;
    int index = model->indexOfDir(dirPath);
    view->insertItem(index);
}

bool DirectoryPresenter::showDirs() {
    return mShowDirs;
}

void DirectoryPresenter::setShowDirs(bool mode) {
    if(mode == mShowDirs)
        return;
    mShowDirs = mode;
    populateView();
}

QList<QString> DirectoryPresenter::selectedPaths() const {
    QList<QString> paths;
    if(!view)
        return paths;
    // const local: iterating a by-value temporary directly would detach it
    const QList<int> selection = view->selection();
    if(mShowDirs) {
        for(const int i : selection) {
            if(i < model->dirCount())
                paths << model->dirPathAt(i);
            else
                paths << model->filePathAt(i - model->dirCount());
        }
    } else {
        for(const int i : selection) {
            paths << model->filePathAt(i);
        }
    }
    return paths;
}

void DirectoryPresenter::generateThumbnails(const QList<int> &indexes, int size, bool crop, bool force) {
    if(!view || !model)
        return;
    thumbnailer.clearTasks();
    if(!mShowDirs) {
        for(int i : indexes)
            thumbnailer.getThumbnailAsync(model->filePathAt(i), size, crop, force);
        return;
    }
    for(int i : indexes) {
        if(i < model->dirCount()) {
            // tmp ------------------------------------------------------------
            // gen thumb for a directory
            // TODO: optimize & move dir icon loading to shared res; then overlay
            // the mini-thumbs on top (similar to dolphin)
            QSvgRenderer svgRenderer;
            svgRenderer.load(QString(":/res/icons/common/other/folder32-scalable.svg"));
            int factor = (size * 0.90f) / svgRenderer.defaultSize().width();
            QPixmap *pixmap = new QPixmap(svgRenderer.defaultSize() * factor);
            pixmap->fill(Qt::transparent);
            QPainter pixPainter(pixmap);
            svgRenderer.render(&pixPainter);
            pixPainter.end();

            ImageLib::recolor(*pixmap, settings->colorScheme().icons);

            std::shared_ptr<Thumbnail> thumb(new Thumbnail(model->dirNameAt(i),
                                                           "Folder",
                                                           size,
                                                           std::shared_ptr<QPixmap>(pixmap)));
            // ^----------------------------------------------------------------
            view->setThumbnail(i, thumb);
        } else {
            QString path = model->filePathAt(i - model->dirCount());
            thumbnailer.getThumbnailAsync(path, size, crop, force);
        }
    }
}

void DirectoryPresenter::onThumbnailReady(std::shared_ptr<Thumbnail> thumb, const QString &filePath) {
    if(!view || !model)
        return;
    int index = model->indexOfFile(filePath);
    if(index == -1)
        return;
    view->setThumbnail(mShowDirs ? model->dirCount() + index : index, thumb);
}

void DirectoryPresenter::onItemActivated(int absoluteIndex) {
    if(!model)
        return;
    if(!mShowDirs) {
        emit fileActivated(model->filePathAt(absoluteIndex));
        return;
    }
    if(absoluteIndex < model->dirCount())
        emit dirActivated(model->dirPathAt(absoluteIndex));
    else
        emit fileActivated(model->filePathAt(absoluteIndex - model->dirCount()));
}

void DirectoryPresenter::onDraggedOut() {
    emit draggedOut(selectedPaths());
}

void DirectoryPresenter::onDraggedOver(int index) {
    if(!model || view->selection().contains(index))
        return;
    if(showDirs() && index < model->dirCount())
        view->setDragHover(index);

}

void DirectoryPresenter::onDroppedInto(const QMimeData *data, QObject *source, int targetIndex) {
    if(!data->hasUrls())
        return;

    // ignore drops into selected / current folder when we are the source of dropEvent
    if(source && (view->selection().contains(targetIndex) || targetIndex == -1) )
        return;
    // ignore drops into a file
    // todo: drop into a current dir when target is a file
    if(showDirs() && targetIndex >= model->dirCount())
        return;

    // convert urls to qstrings
    QStringList pathList;
    QList<QUrl> urlList = data->urls();
    for(int i = 0; i < urlList.size(); ++i)
        pathList.append(urlList.at(i).toLocalFile());

    // get target dir path
    QString destDir;
    if(showDirs() && targetIndex < model->dirCount())
       destDir = model->dirPathAt(targetIndex);
    if(destDir.isEmpty()) // fallback to the current dir
        destDir = model->directoryPath();
    pathList.removeAll(destDir); // remove target dir from source list

    // pass to core
    emit droppedInto(pathList, destDir);
}

void DirectoryPresenter::selectAndFocus(const QString &path) {
    if(!model || !view || path.isEmpty())
        return;
    if(model->containsDir(path) && showDirs()) {
        int dirIndex = model->indexOfDir(path);
        view->select(dirIndex);
        view->focusOn(dirIndex);
    } else if(model->containsFile(path)) {
        int fileIndex = showDirs() ? model->indexOfFile(path) + model->dirCount() : model->indexOfFile(path);
        view->select(fileIndex);
        view->focusOn(fileIndex);
    }
}

void DirectoryPresenter::selectAndFocus(int absoluteIndex) {
    if(!model || !view)
        return;
    view->select(absoluteIndex);
    view->focusOn(absoluteIndex);
}
