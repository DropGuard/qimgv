## SOURCES FOR QIMGV COMPONENTS

target_sources(qimgv_nongui PRIVATE
    directorymodel.cpp
    directorypresenter.cpp

    scriptmanager/scriptmanager.cpp
    actionmanager/actionmanager.cpp

    cache/cache.cpp
    cache/cacheitem.cpp
    cache/thumbnailcache.cpp

    loader/loader.cpp
    loader/loaderrunnable.cpp

    scaler/scaler.cpp
    scaler/scalerrunnable.cpp

    thumbnailer/thumbnailer.cpp
    thumbnailer/thumbnailerrunnable.cpp

    directorymanager/directorymanager.cpp
    directorymanager/enginebridge.cpp
)

