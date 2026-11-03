#pragma once

#include <QObject>
#include <QRunnable>
#include <optional>
#include "utils/imagefactory.h"

class LoaderRunnable: public QObject, public QRunnable
{
    Q_OBJECT
public:
    LoaderRunnable(QString _path, bool _scaled);
    void run();
private:
    QString path;
    bool scaled;
signals:
    void finished(std::optional<std::shared_ptr<Image>>, QString, bool scaled);
    void failed(QString);
};

// crosses the pool thread -> Loader connection as a queued call, so the payload
// needs a metatype the same way std::shared_ptr<Image> does
Q_DECLARE_METATYPE(std::optional<std::shared_ptr<Image>>)
