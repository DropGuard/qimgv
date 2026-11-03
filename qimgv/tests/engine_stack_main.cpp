// One binary, one class per domain: see engine_stack_fixture.h for what
// the classes share. Order here is the order QTest reports them in.
#include <QTest>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QStandardPaths>

int runListingTests(int argc, char **argv);
int runWireFormatTests(int argc, char **argv);
int runFolderBoundaryTests(int argc, char **argv);
int runDecodeTests(int argc, char **argv);
int runSettingsTableTests(int argc, char **argv);
int runFileLogTests(int argc, char **argv);
int runMachineryTests(int argc, char **argv);
int runImageOpsTests(int argc, char **argv);
int runInfraTests(int argc, char **argv);

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    // one sweep per process, at the point where nothing is watched yet
    QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)
         + "/qimgv-engine-stack-test").removeRecursively();
    int failures = 0;
    int (*const runs[])(int, char **) = {
        runListingTests,
        runWireFormatTests,
        runFolderBoundaryTests,
        runDecodeTests,
        runSettingsTableTests,
        runFileLogTests,
        runMachineryTests,
        runImageOpsTests,
        runInfraTests,
    };
    for(auto run : runs)
        failures += run(argc, argv) != 0 ? 1 : 0;
    return failures;
}
