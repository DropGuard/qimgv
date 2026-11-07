#pragma once

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>
#include "gui/idirectoryview.h"
#include "components/thumbnailer/thumbnailer.h"
#include "directorymodel.h"
#include "sharedresources.h"
#include <QMimeData>

//tmp
#include <QtSvg/QSvgRenderer>

class DirectoryPresenter : public QObject {
    Q_OBJECT
public:
    explicit DirectoryPresenter(WorkerPool &pool, QObject *parent = nullptr);

    // The view is an IDirectoryView, which is not a QObject, so a
    // pointer-to-member connect cannot be written against the interface (measured:
    // &IDirectoryView::itemActivated does not compile). It is written against the
    // concrete type instead, which every caller already has: MW hands out
    // FolderViewProxy* and ThumbnailStripProxy*, and both declare these five
    // signals itself. That makes the compiler check every signal/slot pair, and
    // it retires the SIGNAL()/SLOT() strings this used to be - strings that also
    // forced disconnectView to mirror all five signatures by hand.
    template<typename View>
    void setView(View *newView) {
        if(view)
            return;                        // the binding is already live
        view = newView;
        viewSender = newView;              // a QPointer: it goes null with the widget
        if(model)
            view->populate(mShowDirs ? model->totalCount() : model->fileCount());

        connect(newView, &View::itemActivated, this, &DirectoryPresenter::onItemActivated);
        connect(newView, &View::thumbnailsRequested, this, &DirectoryPresenter::generateThumbnails);
        connect(newView, &View::draggedOut, this, &DirectoryPresenter::onDraggedOut);
        connect(newView, &View::draggedOver, this, &DirectoryPresenter::onDraggedOver);
        connect(newView, &View::droppedInto, this, &DirectoryPresenter::onDroppedInto);

        // The teardown names the same five member pointers the connects above
        // used, so there is no second list of signatures that can drift out of
        // agreement with the first. The QPointer test keeps a dead view from
        // being handed to disconnect() at all: Qt has already dropped the
        // connections by then, so this is the contract made explicit rather
        // than a crash observed (ASan flagged nothing without it).
        viewTeardown = [this, newView]() {
            if(!viewSender)
                return;
            disconnect(newView, &View::itemActivated, this, &DirectoryPresenter::onItemActivated);
            disconnect(newView, &View::thumbnailsRequested, this, &DirectoryPresenter::generateThumbnails);
            disconnect(newView, &View::draggedOut, this, &DirectoryPresenter::onDraggedOut);
            disconnect(newView, &View::draggedOver, this, &DirectoryPresenter::onDraggedOver);
            disconnect(newView, &View::droppedInto, this, &DirectoryPresenter::onDroppedInto);
        };
    }
    void setModel(std::shared_ptr<DirectoryModel> newModel);
    void unsetModel();

    void selectAndFocus(int index);
    void selectAndFocus(const QString &path);

    void onFileRemoved(const QString &filePath, int index);
    void onFileAdded(const QString &filePath);
    void onFileModified(const QString &filePath);
    void onFileRenamed(const QString &filePath, const QString &newPath, int oldIndex);

    void onDirRemoved(const QString &dirPath, int index);
    void onDirAdded(const QString &dirPath);
    void onDirRenamed(const QString &dirPath, const QString &newPath, int oldIndex);

    bool showDirs();
    void setShowDirs(bool mode);

    QList<QString> selectedPaths() const;


signals:
    void dirActivated(const QString &dirPath);
    void fileActivated(const QString &filePath);
    void draggedOut(QList<QString>);
    void droppedInto(QList<QString>, QString);

public slots:
    void disconnectView();
    void reloadModel();

private slots:
    void generateThumbnails(const QList<int> &, int, bool, bool);
    void onThumbnailReady(std::shared_ptr<Thumbnail> thumb, const QString &filePath);
    void populateView();
    void onItemActivated(int absoluteIndex);
    void onDraggedOut();
    void onDraggedOver(int index);

    void onDroppedInto(const QMimeData *data, QObject *source, int targetIndex);
private:
    // injected from Core; declared before the thumbnailer that runs on it so
    // the destruction order keeps the pool alive through the thumbnailer's
    // drain
    WorkerPool &pool;
    // observes; the Qt parent tree owns the view widget
    IDirectoryView *view = nullptr;
    // the same widget, seen as the QObject it also is: the teardown needs to
    // ask whether it is still there, and QPointer answers that truthfully
    QPointer<QObject> viewSender;
    std::function<void()> viewTeardown;
    std::shared_ptr<DirectoryModel> model = nullptr;
    Thumbnailer thumbnailer;
    bool mShowDirs;
};
