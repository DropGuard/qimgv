#include "folderviewproxy.h"

FolderViewProxy::FolderViewProxy(QWidget *parent)
    : QWidget(parent),
      folderView(nullptr)
{
    stateBuf.sortingMode = settings->sortingMode();
    layout.setContentsMargins(0,0,0,0);
}

void FolderViewProxy::init() {
    qApp->processEvents(); // chew through events in case we have something that alters stateBuf in queue
    QMutexLocker ml(&m);
    if(folderView)
        return;
    folderView = new FolderView();
    folderView->setParent(this);
    ml.unlock();
    layout.addWidget(folderView);
    this->setFocusProxy(folderView);
    this->setLayout(&layout);

    connect(folderView, &FolderView::itemActivated, this, &FolderViewProxy::itemActivated);
    connect(folderView, &FolderView::thumbnailsRequested, this, &FolderViewProxy::thumbnailsRequested);
    connect(folderView, &FolderView::sortingSelected, this, &FolderViewProxy::sortingSelected);
    connect(folderView, &FolderView::showFoldersChanged, this, &FolderViewProxy::showFoldersChanged);
    connect(folderView, &FolderView::directorySelected, this, &FolderViewProxy::directorySelected);
    connect(folderView, &FolderView::draggedOut, this, &FolderViewProxy::draggedOut);
    connect(folderView, &FolderView::copyUrlsRequested, this, &FolderViewProxy::copyUrlsRequested);
    connect(folderView, &FolderView::moveUrlsRequested, this, &FolderViewProxy::moveUrlsRequested);
    connect(folderView, &FolderView::droppedInto, this, &FolderViewProxy::droppedInto);
    connect(folderView, &FolderView::draggedOver, this, &FolderViewProxy::draggedOver);

    folderView->show();

    // apply buffer
    if(!stateBuf.directory.isEmpty())
        folderView->setDirectoryPath(stateBuf.directory);
    folderView->onFullscreenModeChanged(stateBuf.fullscreenMode);
    folderView->populate(stateBuf.itemCount);
    folderView->select(stateBuf.selection);
    // wait till layout stuff happens
    // before calling focusOn()
    qApp->processEvents();
    folderView->focusOnSelection();
    folderView->onSortingChanged(stateBuf.sortingMode);
}

void FolderViewProxy::populate(int count) {
    QMutexLocker ml(&m);
    stateBuf.itemCount = count;
    if(folderView) {
        ml.unlock();
        folderView->populate(stateBuf.itemCount);
    } else {
        stateBuf.selection.clear();
    }
}

void FolderViewProxy::setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) {
    if(folderView) {
        folderView->setThumbnail(pos, thumb);
    }
}

void FolderViewProxy::select(QList<int> indices) {
    if(folderView) {
        folderView->select(indices);
    } else {
        stateBuf.selection = indices;
    }
}

void FolderViewProxy::select(int index) {
    if(folderView) {
        folderView->select(index);
    } else {
        stateBuf.selection.clear();
        stateBuf.selection << index;
    }
}

QList<int> FolderViewProxy::selection() {
    if(folderView) {
        return folderView->selection();
    } else {
        return stateBuf.selection;
    }
}

void FolderViewProxy::focusOn(int index) {
    if(folderView) {
        folderView->focusOn(index);
    }
}

void FolderViewProxy::focusOnSelection() {
    if(folderView) {
        folderView->focusOnSelection();
    }
}

void FolderViewProxy::setDirectoryPath(QString path) {
    if(folderView) {
        folderView->setDirectoryPath(path);
    } else {
        stateBuf.directory = path;
    }
}

void FolderViewProxy::insertItem(int index) {
    if(folderView) {
        folderView->insertItem(index);
    } else {
        stateBuf.itemCount++;
    }
}

void FolderViewProxy::removeItem(int index) {
    if(folderView) {
        folderView->removeItem(index);
    } else {
        stateBuf.itemCount--;
        stateBuf.selection.removeAll(index);
        for(int i=0; i < stateBuf.selection.count(); i++) {
            if(stateBuf.selection[i] > index)
                stateBuf.selection[i]--;
        }
        if(!stateBuf.selection.count())
            stateBuf.selection << ((index >= stateBuf.itemCount) ? stateBuf.itemCount - 1 : index);
    }
}

void FolderViewProxy::reloadItem(int index) {
    if(folderView)
        folderView->reloadItem(index);
}

// No row state but the selection lives in the buffer, and init() replays
// exactly that — so the move has to reach it even before the view exists.
void FolderViewProxy::renameItem(int oldIndex, int newIndex, const QString &name) {
    if(folderView) {
        folderView->renameItem(oldIndex, newIndex, name);
    } else if(oldIndex != newIndex) {
        for(int i = 0; i < stateBuf.selection.count(); i++) {
            if(stateBuf.selection[i] == oldIndex)
                stateBuf.selection[i] = newIndex;
            else if(oldIndex < newIndex && stateBuf.selection[i] > oldIndex && stateBuf.selection[i] <= newIndex)
                stateBuf.selection[i]--;
            else if(newIndex < oldIndex && stateBuf.selection[i] >= newIndex && stateBuf.selection[i] < oldIndex)
                stateBuf.selection[i]++;
        }
    }
}

void FolderViewProxy::setDragHover(int index) {
    if(folderView)
        folderView->setDragHover(index);
}

void FolderViewProxy::addItem() {
    if(folderView) {
        folderView->addItem();
    } else {
        stateBuf.itemCount++;
    }
}

void FolderViewProxy::onFullscreenModeChanged(bool mode) {
    if(folderView) {
        folderView->onFullscreenModeChanged(mode);
    } else {
        stateBuf.fullscreenMode = mode;
    }
}

void FolderViewProxy::onSortingChanged(SortingMode mode) {
    if(folderView) {
        folderView->onSortingChanged(mode);
    } else {
        stateBuf.sortingMode = mode;
    }
}

void FolderViewProxy::showEvent(QShowEvent *event) {
    init();
    QWidget::showEvent(event);
}
