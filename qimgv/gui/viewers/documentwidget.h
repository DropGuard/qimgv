#pragma once

#include <memory>
#include <QBoxLayout>
#include "gui/customwidgets/floatingwidgetcontainer.h"
#include "gui/viewers/viewerwidget.h"
#include "gui/panels/mainpanel/mainpanel.h"
#include "gui/panels/infobar/infobarproxy.h"

// TODO: use a template here?

class DocumentWidget : public FloatingWidgetContainer {
public:
    DocumentWidget(ViewerWidget *viewWidget, InfoBarProxy *infoBar, QWidget* parent = nullptr);
    ViewerWidget *viewWidget();
    ThumbnailStripProxy *thumbPanel();
    void setFocus();
    void hideFloatingPanel();
    void hideFloatingPanel(bool animated);
    void setPanelEnabled(bool mode);
    bool panelEnabled();
    void setupMainPanel();
    void setInteractionEnabled(bool mode);
    void allowPanelInit();

public slots:
    void onFullscreenModeChanged(bool mode);

private slots:
    void setPanelPinned(bool mode);
    bool panelPinned();
    void readSettings();

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    QBoxLayout *layout, *layoutRoot;
    ViewerWidget *mViewWidget = nullptr;
    InfoBarProxy *mInfoBar = nullptr;
    MainPanel *mainPanel = nullptr;
    bool mPanelPinned, mPanelEnabled, mPanelFullscreenOnly, avoidPanelFlag, mIsFullscreen, mInteractionEnabled, mAllowPanelInit;
};
