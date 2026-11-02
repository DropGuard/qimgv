#include "cmdoptionsrunner.h"

#include <QCoreApplication>
#include <QDebug>

void CmdOptionsRunner::showBuildOptions() {
    QStringList features;
#ifdef USE_MPV
    features << "USE_MPV";
#endif
#ifdef USE_EXIV2
    features << "USE_EXIV2";
#endif
#ifdef USE_KDE_BLUR
    features << "USE_KDE_BLUR";
#endif
#ifdef USE_OPENCV
    features << "USE_OPENCV";
#endif
    qDebug() << "\nEnabled build options:";
    if(!features.count())
        qDebug() << "   --";
    for(int i = 0; i < features.count(); i++)
        qDebug() << "   " << features.at(i);
    QCoreApplication::quit();
}
