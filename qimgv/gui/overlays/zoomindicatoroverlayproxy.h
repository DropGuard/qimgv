#pragma once

#include <QPointer>
#include "gui/overlays/zoomindicatoroverlay.h"

struct ZoomIndicatorOverlayStateBuffer {
    qreal scale = 1.0;
};

class ZoomIndicatorOverlayProxy {
public:
    explicit ZoomIndicatorOverlayProxy(FloatingWidgetContainer *parent = nullptr);
    ~ZoomIndicatorOverlayProxy();
    void init();
    void show();
    void show(int duration);
    void hide();
    void setScale(qreal scale);

private:
    FloatingWidgetContainer *container;
    // QPointer rather than a raw handle: the tree owns the overlay, so it can
    // die first and the proxy has to see that instead of dereferencing it
    QPointer<ZoomIndicatorOverlay> overlay;
    ZoomIndicatorOverlayStateBuffer stateBuf;
};
