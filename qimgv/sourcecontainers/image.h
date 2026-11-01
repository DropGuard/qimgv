#pragma once

#include <QString>
#include <QDebug>
#include <QPixmap>
#include <memory>
#include "utils/imagelib.h"
#include "utils/stuff.h"
#include "sourcecontainers/documentinfo.h"

class Image {
public:
    Image(QString);
    Image(std::unique_ptr<DocumentInfo>);
    virtual ~Image() = 0;
    virtual std::unique_ptr<QPixmap> getPixmap() = 0;
    // two-tier decode: true when the decoded image is a scaled (screen-res)
    // tier-1 result that still needs a full-res upgrade
    virtual bool isScaledResult() const { return false; }
    virtual std::shared_ptr<const QImage> getImage() = 0;
    DocumentType type() const;
    QString filePath() const;
    virtual int height() = 0;
    virtual int width() = 0;
    virtual QSize size() = 0;
    bool isLoaded() const;
    virtual bool save() = 0;
    virtual bool save(QString destPath) = 0;
    QString fileName() const;
    QString baseName() const;
    bool isEdited() const;
    qint64 fileSize() const;
    QDateTime lastModified() const;
    QMap<QString, QString> getExifTags();

protected:
    virtual void load() = 0;
    std::unique_ptr<DocumentInfo> mDocInfo;
    bool mLoaded, mEdited;
    QString mPath;
    QSize resolution;
};
