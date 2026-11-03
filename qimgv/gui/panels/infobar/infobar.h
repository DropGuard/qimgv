#pragma once

#include <QWidget>
#include <QWheelEvent>
#include <QDebug>

#include <QPainter>
#include <QStyleOption>

namespace Ui {
class InfoBar;
}

class InfoBar : public QWidget
{
    Q_OBJECT

public:
    explicit InfoBar(QWidget *parent = nullptr);
    ~InfoBar() override;

public slots:
    void setInfo(QString position, QString fileName, QString info);
protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    Ui::InfoBar *ui;
};
