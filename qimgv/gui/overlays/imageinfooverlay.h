#pragma once

#include "gui/customwidgets/overlaywidget.h"
#include "gui/customwidgets/entryinfoitem.h"
#include <QWheelEvent>

namespace Ui {
class ImageInfoOverlay;
}

class ImageInfoOverlay : public OverlayWidget
{
    Q_OBJECT

public:
    explicit ImageInfoOverlay(FloatingWidgetContainer *parent = nullptr);
    ~ImageInfoOverlay() override;
    void setExifInfo(QMap<QString, QString>);

public slots:
    void show();

protected:
    void wheelEvent(QWheelEvent *event) override;
private:
    Ui::ImageInfoOverlay *ui;
    QList<EntryInfoItem*> entries;
    QLabel entryStub;
};
