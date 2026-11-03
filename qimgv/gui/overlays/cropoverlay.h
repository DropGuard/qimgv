#pragma once

#include "gui/customwidgets/floatingwidget.h"
#include <QDebug>
#include <QPainter>
#include <QPaintEvent>
#include <QColor>

enum CursorAction {
    NO_DRAG,          // 0
    SELECTION_START,  // 1
    DRAG_SELECT,      // 2
    DRAG_MOVE,        // 3
    DRAG_LEFT,        // 4
    DRAG_RIGHT,       // 5
    DRAG_TOP,         // 6
    DRAG_BOTTOM,      // 7
    DRAG_TOPLEFT,     // 8
    DRAG_TOPRIGHT,    // 9
    DRAG_BOTTOMLEFT,  // 10
    DRAG_BOTTOMRIGHT  // 11
};

class CropOverlay : public FloatingWidget
{
    Q_OBJECT
public:
    explicit CropOverlay(FloatingWidgetContainer *parent = nullptr);
    void setImageDrawRect(QRect);
    void setImageRealSize(QSize);
    void setButtonText(QString text);
    void setImageScale(float scale);
    void clearSelection();

signals:
    void positionChanged(float x, float y);
    void selectionChanged(QRect);
    void escPressed();
    void cropDefault();
    void cropSave();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    QPoint startPos, endPos, moveStartPos, resizeAnchor;
    QRect imageRect, imageDrawRect, imageDrawRectDpi, selectionRect, selectionDrawRect, selectionDrawRectDpi, handles[8];
    bool lockAspectRatio;
    float scale;
    QBrush brushInactiveTint, brushDarkGray, brushGray, brushLightGray;
    QRectF handlesDpi[8];
    int handleSize;
    CursorAction cursorAction;
    QPen selectionOutlinePen;
    qreal dpr;
    QPointF ar;

    QPoint setInsidePoint(QPoint, QRect);
    QRect placeInside(QRect what, QRect where);
    void drawSelection(QPainter*);
    void drawHandles(QBrush&, QPainter*);
    void updateHandlePositions();
    void prepareDrawElements();
    CursorAction hoverTarget(QPoint pos);
    void resizeSelection(QPoint d);
    void resizeSelectionAR(QPoint d);
    void resizeSelectionFree(QPoint d);
    void recalculateGeometry() override;
    QPoint mapPointToImage(QPoint p);
    void updateSelectionDrawRect();
    void setCursorAction(CursorAction action);
    void setResizeAnchor(CursorAction action);
    bool hasSelection();
public slots:
    void hide();
    void onSelectionOutsideChange(QRect selection);
    void selectAll();
    void setAspectRatio(QPointF);
    void setLockAspectRatio(bool mode);
};
