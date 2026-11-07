#pragma once

#include "gui/panels/mainpanel/thumbnailstrip.h"
#include <QMutexLocker>

struct ThumbnailStripStateBuffer {
    QList<int> selection;
    int itemCount = 0;
};

class ThumbnailStripProxy : public QWidget, public IDirectoryView {
    Q_OBJECT
    Q_INTERFACES(IDirectoryView)
public:
    ThumbnailStripProxy(QWidget *parent = nullptr);
    void init();
    bool isInitialized();
    QSize itemSize();
    void readSettings();

public slots:
    void populate(int) override;
    void setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) override;
    void select(QList<int>) override;
    void select(int) override;
    QList<int> selection() override;
    void focusOn(int) override;
    void focusOnSelection() override;
    void insertItem(int index) override;
    void removeItem(int index) override;
    void reloadItem(int index) override;
    void renameItem(int oldIndex, int newIndex, const QString &name) override;
    void setDragHover(int index) override;
    void setDirectoryPath(QString path) override;
    void addItem();

protected:
    void showEvent(QShowEvent *event) override;

signals:
    void itemActivated(int) override;
    void thumbnailsRequested(QList<int>, int, bool, bool) override;
    void draggedOut() override;
    void droppedInto(const QMimeData*, QObject*, int) override;
    void draggedOver(int) override;

private:
    ThumbnailStrip *thumbnailStrip = nullptr;
    QVBoxLayout layout;
    ThumbnailStripStateBuffer stateBuf;
    QMutex m;
};
