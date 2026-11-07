#pragma once

#include <QPointer>
#include "gui/overlays/imageinfooverlay.h"

struct ImageInfoOverlayStateBuffer {
    QMap<QString, QString> info;
};

class ImageInfoOverlayProxy {
public:
    explicit ImageInfoOverlayProxy(FloatingWidgetContainer *parent = nullptr);
    ~ImageInfoOverlayProxy();
    void init();
    void show();
    void hide();

    void setExifInfo(const QMap<QString, QString>& info);
    bool isHidden();
private:
    FloatingWidgetContainer *container;
    QPointer<ImageInfoOverlay> overlay;
    ImageInfoOverlayStateBuffer stateBuf;
};
