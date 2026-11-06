#pragma once

#include <QCryptographicHash>
#include <QDebug>
#include <QString>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <QtGlobal>

class QImage;

#ifdef Q_OS_WIN32
#include "windows.h"
#endif

enum FileOpResult {
    SUCCESS,
    DESTINATION_FILE_EXISTS,
    DESTINATION_DIR_EXISTS,
    SOURCE_NOT_WRITABLE,
    DESTINATION_NOT_WRITABLE,
    SOURCE_DOES_NOT_EXIST,
    DESTINATION_DOES_NOT_EXIST,
    DIRECTORY_NOT_EMPTY,
    NOTHING_TO_DO, // todo: maybe just return SUCCESS?
    OTHER_ERROR
};

class FileOperations {
public:
    static void copyFileTo(const QString &srcFilePath, const QString &destDirPath, bool force, FileOpResult &result);
    static void moveFileTo(const QString &srcFilePath, const QString &destDirPath, bool force, FileOpResult &result);
    static void rename(const QString &srcFilePath, const QString &newName, bool force, FileOpResult &result);
    static void removeFile(const QString &filePath, FileOpResult &result);
    static void removeDir(const QString &dirPath, bool recursive, FileOpResult &result);
    static void moveToTrash(const QString &filePath, FileOpResult &result);

    static QString decodeResult(const FileOpResult &result);

    // The one implementation of "overwrite a file from memory, and put the old
    // contents back if the write fails". Both ImageStatic::save() and the
    // clipboard import carried their own copy; the copies had already diverged,
    // and the clipboard one still held the rollback bug the other fixed - it
    // deleted the original, then deleted the backup even when the restore
    // itself failed, turning "save failed" into "your file is gone".
    static bool saveWithBackup(const QImage &image, const QString &destPath, int jpegQuality);

private:
    static bool moveToTrashImpl(const QString &path);
    static QString generateHash(const QString &str);
};
