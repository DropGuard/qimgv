#include "video.h"
#include <time.h>

Video::Video(std::unique_ptr<DocumentInfo> _info) : Image(std::move(_info)) {
    Video::load();
}

void Video::load() {
    if(mLoadAttempted)
        return;

    // the ffmpeg/mpv resolution probe that used to live here is retired:
    // it never ran (commented out), and video metadata comes from the
    // container via DocumentInfo

    mLoadAttempted = true;
}

bool Video::save(QString destPath, int) {
    Q_UNUSED(destPath)
    qDebug() << "Saving video is unsupported.";
    return false;
}

std::unique_ptr<QPixmap> Video::getPixmap() {
    qDebug() << "[Video] getPixmap() is not implemented.";
    //TODO: find out some easy way to get frames from video source
    return nullptr;
}

std::shared_ptr<const QImage> Video::getImage() {
    qDebug() << "[Video] getImage() is not implemented.";
    //TODO: find out some easy way to get frames from video source
    return nullptr;
}

int Video::height() {
    return srcHeight;
}

int Video::width() {
    return srcWidth;
}

QSize Video::size() {
    return QSize(srcWidth, srcHeight);
}
