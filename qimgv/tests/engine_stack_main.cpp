// One binary, one class per domain: see engine_stack_fixture.h for what
// the classes share. Order here is the order QTest reports them in.
#include <QTest>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QStandardPaths>

#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

int runListingTests(int argc, char **argv);
int runWireFormatTests(int argc, char **argv);
int runFolderBoundaryTests(int argc, char **argv);
int runDecodeTests(int argc, char **argv);
int runSettingsTableTests(int argc, char **argv);
int runFileLogTests(int argc, char **argv);
int runMachineryTests(int argc, char **argv);
int runWorkerPoolTests(int argc, char **argv);
int runFileOpsTests(int argc, char **argv);
int runImageOpsTests(int argc, char **argv);
int runInfraTests(int argc, char **argv);

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    // This process's own tree, at the point where nothing is watched yet — and
    // the leftovers of runs whose process is gone. Sweeping the whole base was
    // wrong: it deletes the directories a concurrently running instance is
    // watching, which shows up later as a listing that fails or goes stale.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                         + "/qimgv-engine-stack-test";
    const QString mine = base + "/pid" + QString::number(QCoreApplication::applicationPid());
    QDir(mine).removeRecursively();
    QDir(base).mkpath(QFileInfo(mine).fileName());
    const QStringList orphans = QDir(base).entryList(QStringList() << QStringLiteral("pid*"), QDir::Dirs);
    for(const QString &stale : orphans)
        if(!QFile::exists(QStringLiteral("/proc/%1").arg(stale.mid(3))))
            QDir(base + "/" + stale).removeRecursively();
    int failures = 0;
    int (*const runs[])(int, char **) = {
        runListingTests,
        runWireFormatTests,
        runFolderBoundaryTests,
        runDecodeTests,
        runSettingsTableTests,
        runFileLogTests,
        runMachineryTests,
        runWorkerPoolTests,
        runFileOpsTests,
        runImageOpsTests,
        runInfraTests,
    };
    // One process per class. Not for parallelism and not primarily for crash
    // isolation: QtTest starts a WatchDog QThread per class and joins it at
    // class end through QThread::wait, i.e. pthread_clockjoin_np — the one
    // join TSan does not intercept, same root cause as the QThreadPool waves
    // (see the QIMGV_SANITIZERS block). Ten classes in one process means ten
    // registry-live thread ids for the next class's pthread_create to collide
    // with; one process per class gives every watchdog a private registry that
    // is abandoned at exit before any pthread_t value can be reused. Per-class
    // LSan attribution at exit is a side effect worth having.
    for(auto run : runs) {
        fflush(nullptr); // fork would duplicate whatever is buffered in stdout
        pid_t pid = fork();
        if(pid == 0)
            return run(argc, argv) != 0 ? 1 : 0;
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}
