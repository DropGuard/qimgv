#pragma once

#include <QImage>
#include <QImageWriter>
#include <QSemaphore>
#include <QCryptographicHash>
#include "image.h"
#include "utils/imagelib.h"
#include <settings.h>
#include <QIcon>

class ImageStatic : public Image {
public:
    ImageStatic(std::unique_ptr<DocumentInfo> _info, bool scaled, const LoadPolicy &policy);
    bool isScaledRequest() const { return mScaledRequest; }
    bool isScaledResult() const override { return mScaledApplied; }
    ~ImageStatic() override;

    std::unique_ptr<QPixmap> getPixmap() override;
    std::shared_ptr<const QImage> getSourceImage();
    std::shared_ptr<const QImage> getImage() override;

    int height() override;
    int width() override;
    QSize size() override;

    bool setEditedImage(std::unique_ptr<const QImage> imageEditedNew);
    bool discardEditedImage();

public slots:
    void crop(QRect newRect);
    bool save(QString destPath, int jpegQuality) override;

private:
    bool mScaledRequest = false;
    bool mScaledApplied = false;
    int mAllocationLimitMb;
    void load() override;
    std::shared_ptr<const QImage> image, imageEdited;
    void loadGeneric();
    void loadICO();
};
