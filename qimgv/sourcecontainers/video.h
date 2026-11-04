#pragma once

#include <QImage>
#include <QPixmap>
#include <QProcess>
#include <QFile>
#include "image.h"

class Video : public Image {
public:
    Video(std::unique_ptr<DocumentInfo> _info);

    std::unique_ptr<QPixmap> getPixmap() override;
    std::shared_ptr<const QImage> getImage() override;
    int height() override;
    int width() override;
    QSize size() override;

public slots:
    bool save(QString destPath, int jpegQuality) override;


private:
    void load() override;

    uint srcWidth = 0;
    uint srcHeight = 0;
};
