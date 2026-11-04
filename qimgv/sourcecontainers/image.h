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
    virtual bool save(QString destPath, int jpegQuality) = 0;
    QString fileName() const;
    QString baseName() const;
    bool isEdited() const;
    qint64 fileSize() const;
    QDateTime lastModified() const;
    QMap<QString, QString> getExifTags();

protected:
    virtual void load() = 0;
    std::unique_ptr<DocumentInfo> mDocInfo;
    // "load() has run", NOT "we got pixels": a file we try to open and fail to
    // decode still counts as attempted. The honest name matters because getImage()
    // is the only question that distinguishes the two, and Core::guiSetImage asks it.
    bool mLoadAttempted, mEdited;
    QString mPath;
    QSize resolution;
};
