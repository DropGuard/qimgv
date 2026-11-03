#pragma once

#include <QPushButton>
#include "shortcutbuilder.h"
#include "utils/actions.h"

class KeySequenceEdit : public QPushButton {
    Q_OBJECT
public:
    KeySequenceEdit(QWidget *parent);
    QString sequence();    
    void setText(const QString &text);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

    void mouseReleaseEvent(QMouseEvent *e) override;
    bool focusNextPrevChild(bool) override;
private:
    void processEvent(QEvent *e);
    QString mSequence;

signals:
    void edited();
};
