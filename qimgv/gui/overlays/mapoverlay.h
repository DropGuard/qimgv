#pragma once

#include <QWidget>
#include <QPainter>
#include <QPen>
#include <QDebug>
#include <QPaintEvent>

class MapOverlay : public QWidget
{
    Q_OBJECT
    Q_PROPERTY (float opacity READ opacity WRITE setOpacity)
    Q_PROPERTY (int y READ y WRITE setY)
public:
    enum Location 
    {
        LeftTop,
        RightTop,
        RightBottom,
        LeftBottom
    };
    
    explicit MapOverlay(QWidget *parent = nullptr);
    ~MapOverlay() override;
    void resize(int size);
    int size() const;
    void animateVisible(bool visible);
    
    void setOpacity(float opacity);
    float opacity() const;

    void enableVisibility(bool);
    
    void setLocation(Location l);
    Location location() const;
    
    void setMargin(int margin);
    int margin() const;
    
    void setY(int y);
    int y() const;
    
    /**
     * @brief Updating navigation map
     * calculates outer(image) and inner(view area) squares.
     */
    void updateMap(const QRectF& drawingRect);
    
    /**
     * Recalculates map position on resize
     */
    void updatePosition();
    
    /**
     * Unit test functions
     */
    QSizeF inner() const;
    QSizeF outer() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent *) override;
    virtual void enterEvent(QEvent *);
    
private:
    bool visibilityEnabled = false, imageDoesNotFit = false;
    class MapOverlayPrivate;
    MapOverlayPrivate* d;
};
