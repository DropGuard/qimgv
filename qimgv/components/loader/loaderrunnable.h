#pragma once

#include <QObject>
#include <QRunnable>
#include <optional>
#include "utils/imagefactory.h"

class LoaderRunnable: public QObject, public QRunnable
{
    Q_OBJECT
public:
    LoaderRunnable(QString _path, bool _scaled, const LoadPolicy &policy);
    void run() override;
private:
    QString path;
    bool scaled;
    LoadPolicy policy;
signals:
    void finished(std::optional<std::shared_ptr<Image>>, QString, bool scaled);
    void failed(QString);
};
