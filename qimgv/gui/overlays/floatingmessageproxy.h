#pragma once

#include <QPointer>
#include "gui/overlays/floatingmessage.h"

class FloatingMessageProxy
{
public:
    FloatingMessageProxy(FloatingWidgetContainer *parent);
    ~FloatingMessageProxy();
    void showMessage(QString text, FloatingMessageIcon icon, int duration);
    void showMessage(QString text, FloatingWidgetPosition position, FloatingMessageIcon icon, int duration);
    void init();
private:
    FloatingWidgetContainer *container;
    QPointer<FloatingMessage> overlay;
};
