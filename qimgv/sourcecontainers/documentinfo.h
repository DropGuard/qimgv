#pragma once

#include <QString>
#include <QSize>
#include <QUrl>
#include <QMimeDatabase>
#include <QDebug>
#include <QFileInfo>
#include <QDateTime>
#include <cmath>
#include <cstring>
#include "utils/stuff.h"
#include "settings.h"
#include "sourcecontainers/loadpolicy.h"

#ifdef USE_EXIV2

#include <exiv2/exiv2.hpp>
#include <iostream>
#include <iomanip>
#include <cassert>

#endif

#include <QImageReader>

enum DocumentType { NONE, STATIC, ANIMATED, VIDEO };

class DocumentInfo {
public:
    // classification consults the CARRIED policy (jxl animation, video
    // playback/formats), never the settings global — this constructor runs
    // on the loader pool
    DocumentInfo(QString path, const LoadPolicy &policy);
    ~DocumentInfo();
    
    QString directoryPath() const;
    QString filePath() const;
    QString fileName() const;
    QString baseName() const;
    qint64 fileSize() const;
    DocumentType type() const;
    QMimeType mimeType() const;

    // file extension (guessed from mime-type)
    QString format() const;
    int exifOrientation() const;

    QDateTime lastModified() const;
    void refresh();
    void loadExifTags();
    QMap<QString, QString> getExifTags();

private:
    QFileInfo fileInfo;
    DocumentType mDocumentType;
    int mOrientation;
    QString mFormat;
    bool exifLoaded;

    // guesses file type from its contents
    // and sets extension
    void detectFormat(const LoadPolicy &policy);
    void loadExifOrientation();
    bool detectAPNG();
    bool detectAnimatedWebP();
    bool detectAnimatedJxl();
    bool detectAnimatedAvif();
    QMap<QString, QString> exifTags;
    QMimeType mMimeType;
};
