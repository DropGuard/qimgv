#pragma once

#include <QObject>
#include <QString>

class CmdOptionsRunner : public QObject {
    Q_OBJECT
public slots:
    void showBuildOptions();
};
