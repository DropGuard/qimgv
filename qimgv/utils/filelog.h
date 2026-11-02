#pragma once

#include <QFile>
#include <QString>
#include <QtGlobal>

// File logging: one session file under <cacheDir>/logs, INFO by default
// (lifecycle lines), QIMGV_DEBUG=1 enables hot lines via qimgv.perf.*.
// Rotates at 5MB per part, max 4 parts, then stops writing (console keeps).
//
// Extracted from main.cpp so the rotation/prune machinery is testable: the
// handler installed by install() only composes the pieces below, and each
// piece can be driven directly with tiny limits in a test process.
namespace FileLog {

// "MM-dd hh:mm:ss.zzz L msg\n" where L is one of "DWCIF" (the same shape
// main.cpp has always written; tests pin it so a silent format change cannot
// break the log-reading habits built on it).
QString formatLine(QtMsgType type, const QString &msg);

// Keeps the newest `keep` files matching "qimgv-*.log*" in dir. Per-session
// files sort by name, so the oldest die first.
void pruneOldSessions(const QString &dir, int keep);

// One session's log file with part rotation. Production limits are 5MB x 4
// parts; tests construct the class with tiny ones and write through the
// boundary. Byte accounting starts at zero after the banner, exactly as the
// main.cpp original counted (only handler writes rotate, not the banner).
class SessionLog {
public:
    SessionLog(qint64 partLimitBytes = 5 * 1024 * 1024, int maxParts = 4);
    bool open(const QString &basePath); // opens "<base>", writes the banner
    bool isWriting() const;             // open, and not yet retired at the cap
    int part() const;
    qint64 bytesInPart() const;
    QString lastError() const;
    void write(const QString &line);

private:
    QFile mFile;
    QString mBase;
    qint64 mPartLimit;
    int mMaxParts;
    qint64 mBytes = 0;
    int mPart = 1;
    bool mStopped = false;
};

// mkpath(dir), prune to the newest 10, open the session file, write the
// banner, enable qimgv.perf.* under QIMGV_DEBUG, install as the Qt message
// handler (the previous handler keeps running: console output survives).
// Returns false if the session file could not be opened; logging then stays
// console-only, which is how main.cpp behaved on the same failure.
bool install(const QString &dir);

} // namespace FileLog
