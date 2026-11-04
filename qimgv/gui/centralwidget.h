#pragma once

#include <QStackedWidget>
#include "gui/folderview/folderviewproxy.h"
#include "gui/viewers/documentwidget.h"
#include "settings.h"


class CentralWidget : public QStackedWidget
{
    Q_OBJECT
public:
    explicit CentralWidget(std::shared_ptr<DocumentWidget> _docWidget, std::shared_ptr<FolderViewProxy> _folderView, QWidget *parent = nullptr);

    ViewMode currentViewMode();
signals:

public slots:
    void showDocumentView();
    void showFolderView();
    void toggleViewMode();

private:
    std::shared_ptr<DocumentWidget> documentView;
    std::shared_ptr<FolderViewProxy> folderView;
    // MODE_FOLDER, not MODE_DOCUMENT: the ctor calls showDocumentView(),
    // which early-returns on MODE_DOCUMENT (the other value is MODE_FOLDERVIEW) — an uninitialized or
    // document-initialized mode would skip the first full setup (UBSan catch)
    ViewMode mode = MODE_FOLDERVIEW;
};
