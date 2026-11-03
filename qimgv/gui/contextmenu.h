#pragma once

#include <QScreen>
#include <QStyleOption>
#include <QTimer>
#include <QPainter>
#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"

namespace Ui {
class ContextMenu;
}

class ContextMenu : public QWidget {
    Q_OBJECT
public:
    explicit ContextMenu(QWidget *parent = nullptr);
    ~ContextMenu() override;
    void setImageEntriesEnabled(bool mode);

public slots:
    void showAt(QPoint pos);
    void setGeometry(QRect geom);

private:
    Ui::ContextMenu *ui;
    QTimer mTimer;

    void fillOpenWithMenu();

signals:
    void showScriptSettings();

protected:
    void mousePressEvent(QMouseEvent*) override;
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;
private slots:
    void switchToMainPage();
    void switchToScriptsPage();
};
