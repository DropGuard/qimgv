#pragma once

#include <QPointer>
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
    void setInfo(const QString& position, const QString& fileName, const QString& info);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    InfoBarStateBuffer stateBuf;
    QPointer<InfoBar> infoBar;
    QVBoxLayout layout;
};
