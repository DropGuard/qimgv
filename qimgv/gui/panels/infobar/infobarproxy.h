#pragma once

#include "gui/panels/infobar/infobar.h"
#include <QVBoxLayout>

struct InfoBarStateBuffer {
    QString position;
    QString fileName;
    QString info;
};

class InfoBarProxy : public QWidget {
    Q_OBJECT
public:
    explicit InfoBarProxy(QWidget *parent = nullptr);
    ~InfoBarProxy() override;

    void init();
public slots:
    void setInfo(QString position, QString fileName, QString info);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    InfoBarStateBuffer stateBuf;
    InfoBar *infoBar;
    QVBoxLayout layout;
};
