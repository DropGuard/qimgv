#pragma once

#include <QObject>
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

    void setView(IDirectoryView *view);
    void setModel(std::shared_ptr<DirectoryModel> newModel);
    void unsetModel();

    void selectAndFocus(int index);
    void selectAndFocus(const QString &path);

    void onFileRemoved(const QString &filePath, int index);
    void onFileRenamed(const QString &fromPath, int indexFrom, const QString &toPath, int indexTo);
    void onFileAdded(const QString &filePath);
    void onFileModified(const QString &filePath);

    void onDirRemoved(const QString &dirPath, int index);
    void onDirRenamed(const QString &fromPath, int indexFrom, const QString &toPath, int indexTo);
    void onDirAdded(const QString &dirPath);

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
    std::shared_ptr<DirectoryModel> model = nullptr;
    Thumbnailer thumbnailer;
    bool mShowDirs;
};
