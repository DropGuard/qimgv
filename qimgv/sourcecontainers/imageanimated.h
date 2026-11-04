#pragma once

#include "image.h"
#include <QMovie>
#include <QTimer>

class ImageAnimated : public Image {
public:
    ImageAnimated(std::unique_ptr<DocumentInfo> _info);
    ~ImageAnimated() override;

    std::unique_ptr<QPixmap> getPixmap() override;
    std::shared_ptr<const QImage> getImage() override;
    std::shared_ptr<QMovie> getMovie();
    int height() override;
    int width() override;
    QSize size() override;

    bool isEditable();
    bool isEdited();

    int frameCount();
public slots:
    bool save(QString destPath, int jpegQuality) override;

signals:
    void frameChanged(QPixmap*);

private:
    void load() override;
    QSize mSize;
    int mFrameCount;
    std::shared_ptr<QMovie> movie;
    void loadMovie();
};
