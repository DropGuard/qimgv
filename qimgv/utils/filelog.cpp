#include "filelog.h"

#include <QDateTime>
#include <QDir>
#include <QLoggingCategory>
#include <cstdio>

namespace FileLog {

static QtMessageHandler gPrevHandler = nullptr;
static SessionLog gSession;

QString formatLine(QtMsgType type, const QString &msg) {
    const char lvl[] = "DWCFI"; // enum order: Debug Warning Critical Fatal Info
    return QString("%1 %2 %3\n")
        .arg(QDateTime::currentDateTime().toString("MM-dd hh:mm:ss.zzz"),
             QString(QChar(lvl[type])), msg);
}

void pruneOldSessions(const QString &dir, int keep) {
    if(keep < 1)
        return;
    // the legacy glob ages out the logs the identity migration did not carry
    const QStringList logs = QDir(dir).entryList(QStringList() << "impulse-*.log*" << "qimgv-*.log*",
                                                 QDir::Files, QDir::Name);
    for(int i = 0; i + keep < logs.size(); ++i)
        QFile::remove(dir + "/" + logs.at(i));
}

SessionLog::SessionLog(qint64 partLimitBytes, int maxParts)
    : mPartLimit(partLimitBytes), mMaxParts(maxParts)
{
}

bool SessionLog::open(const QString &basePath) {
    mBase = basePath;
    mBytes = 0;
    mPart = 1;
    mStopped = false;
    mFile.setFileName(mBase);
    if(!mFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
    mFile.write("=== session start ===\n");
    mFile.flush();
    return true;
}

bool SessionLog::isWriting() const {
    return mFile.isOpen() && !mStopped;
}

int SessionLog::part() const {
    return mPart;
}

qint64 SessionLog::bytesInPart() const {
    return mBytes;
}

QString SessionLog::lastError() const {
    return mFile.errorString();
}

void SessionLog::write(const QString &line) {
    if(mStopped || !mFile.isOpen())
        return;
    mBytes += mFile.write(line.toUtf8());
    mFile.flush();
    if(mBytes >= mPartLimit) {
        if(mPart >= mMaxParts) {
            mStopped = true; // console keeps working
            return;
        }
        ++mPart;
        mFile.close();
        mFile.setFileName(mBase + QString(".part%1").arg(mPart));
        if(!mFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            mStopped = true; // rotation target unopenable: stop writing, console keeps working
        mBytes = 0;
    }
}

static void fileLogHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
    if(gPrevHandler)
        gPrevHandler(type, ctx, msg); // console keeps working
    gSession.write(formatLine(type, msg));
}

bool install(const QString &dir) {
    QDir().mkpath(dir);
    pruneOldSessions(dir, 10);
    const QString base = dir + "/impulse-"
                         + QDateTime::currentDateTime().toString("MMdd-hhmmss") + ".log";
    if(!gSession.open(base)) {
        fprintf(stderr, "[log] open failed: %s\n",
                gSession.lastError().toLocal8Bit().constData());
        return false;
    }
    fprintf(stderr, "[log] handler installed: %s\n", base.toLocal8Bit().constData());
    if(!qEnvironmentVariableIsEmpty("QIMGV_DEBUG"))
        // only our categories — a bare "*.debug = true" would also flood the
        // file with Qt-internal plugin scanning noise
        QLoggingCategory::setFilterRules("qimgv.perf.*.debug = true\n");
    gPrevHandler = qInstallMessageHandler(fileLogHandler);
    return true;
}

} // namespace FileLog
