#include "fsentry.h"

FSEntry::FSEntry() {
}

FSEntry::FSEntry(const QString &filePath) {
    try {
        std::filesystem::directory_entry stdEntry(toStdString(filePath));
        QString fileName = QString::fromStdString(stdEntry.path().filename().generic_string());
        if(stdEntry.is_directory()) {
            this->name = fileName;
            this->path = filePath;
            this->isDirectory = true;
        } else {
            // stat both attributes before touching any member: a throw here
            // must leave the entry fully empty, never half-populated
            std::uintmax_t fileSize = stdEntry.file_size();
            std::filesystem::file_time_type lastWrite = stdEntry.last_write_time();
            this->name = fileName;
            this->path = filePath;
            this->isDirectory = false;
            this->size = fileSize;
            this->modifyTime = lastWrite;
        }
    } catch (const std::filesystem::filesystem_error &err) {
        // the stat lost a race with a delete, or the path is unreadable:
        // stay empty; DirectoryManager::updateFileEntry refuses empty entries
    }
}

FSEntry::FSEntry( QString _path, QString _name, std::uintmax_t _size, std::filesystem::file_time_type _modifyTime, bool _isDirectory)
    : path(_path),
      name(_name),
      size(_size),
      modifyTime(_modifyTime),
      isDirectory(_isDirectory)
{
}
FSEntry::FSEntry( QString _path, QString _name, std::uintmax_t _size, bool _isDirectory)
    : path(_path),
      name(_name),
      size(_size),
      isDirectory(_isDirectory)
{
}
FSEntry::FSEntry( QString _path, QString _name, bool _isDirectory)
    : path(_path),
      name(_name),
      isDirectory(_isDirectory)
{
}
bool FSEntry::operator==(const QString &anotherPath) const {
    return this->path == anotherPath;
}
