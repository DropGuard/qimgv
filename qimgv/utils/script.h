#pragma once

#include <QString>

class Script {
public:
    Script();
    Script(QString _path, bool _blocking);
    QString command;
    bool blocking;
};
