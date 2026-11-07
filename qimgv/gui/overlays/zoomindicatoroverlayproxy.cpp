#include "zoomindicatoroverlayproxy.h"

ZoomIndicatorOverlayProxy::ZoomIndicatorOverlayProxy(FloatingWidgetContainer *parent)
    : container(parent),
      overlay(nullptr)
{
}

ZoomIndicatorOverlayProxy::~ZoomIndicatorOverlayProxy() {
    // The overlay is the container's child, so the tree reclaims it: deleting
    // it from here was a proxy destroying a widget it never owned. The
    // use-after-free that shape allowed is closed by the member being a
    // QPointer, which nulls itself when the overlay dies - pinned by
    // proxyOutlivesItsContainer in core_nav_tests.cpp.
}

void ZoomIndicatorOverlayProxy::show() {
    init();
    overlay->show();
}

void ZoomIndicatorOverlayProxy::show(int duration) {
    init();
    overlay->show(duration);
}

void ZoomIndicatorOverlayProxy::hide() {
    if(overlay)
        overlay->hide();
}

void ZoomIndicatorOverlayProxy::setScale(qreal scale) {
    if(overlay) {
        overlay->setScale(scale);
    } else {
        stateBuf.scale = scale;
    }
}

void ZoomIndicatorOverlayProxy::init() {
    if(overlay)
        return;
    overlay = new ZoomIndicatorOverlay(container);
    overlay->setScale(stateBuf.scale);
}
