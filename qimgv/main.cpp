#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QStyleFactory>
#include <QEvent>

#include "appversion.h"
#include "settings.h"
#include "components/actionmanager/actionmanager.h"
#include "utils/inputmap.h"
#include "utils/actions.h"
#include "utils/cmdoptionsrunner.h"
#include "sharedresources.h"
#include "proxystyle.h"
#include "core.h"

#include "qimgv_engine.h"

#ifdef __APPLE__
#include "macosapplication.h"
#endif

//------------------------------------------------------------------------------
// file logging: one session file under <cacheDir>/logs, INFO by default
// (lifecycle lines), QIMGV_DEBUG=1 enables hot lines via qimgv.perf.*.
// Rotates at 5MB per part, max 4 parts, then stops writing (console keeps).
static QFile gLogFile;
static QString gLogBase;
static qint64 gLogBytes = 0;
static int gLogPart = 1;
static bool gLogStop = false;
static QtMessageHandler gPrevHandler = nullptr;
static const qint64 kLogPartLimit = 5 * 1024 * 1024;
static const int kLogMaxParts = 4;

static void qimgvFileLog(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
    if(gPrevHandler)
        gPrevHandler(type, ctx, msg); // console keeps working
    if(gLogStop || !gLogFile.isOpen())
        return;
    const char lvl[] = "DWCIF"; // Debug Warning Critical Fatal Info
    QString line = QString("%1 %2 %3\n")
        .arg(QDateTime::currentDateTime().toString("MM-dd hh:mm:ss.zzz"),
             QString(QChar(lvl[type])), msg);
    gLogBytes += gLogFile.write(line.toUtf8());
    gLogFile.flush();
    if(gLogBytes >= kLogPartLimit) {
        if(gLogPart >= kLogMaxParts) {
            gLogStop = true;
            return;
        }
        ++gLogPart;
        gLogFile.close();
        gLogFile.setFileName(gLogBase + QString(".part%1").arg(gLogPart));
        gLogFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        gLogBytes = 0;
    }
}

static void setupFileLogging() {
    if(!settings)
        return;
    const QString dir = settings->tmpDir() + "/logs";
    QDir().mkpath(dir);
    // per-session files sort by name; keep the newest 10
    const QStringList logs = QDir(dir).entryList(QStringList() << "qimgv-*.log*", QDir::Files, QDir::Name);
    for(int i = 0; i + 10 < logs.size(); ++i)
        QFile::remove(dir + "/" + logs.at(i));
    gLogBase = dir + "/qimgv-" + QDateTime::currentDateTime().toString("MMdd-hhmmss") + ".log";
    gLogFile.setFileName(gLogBase);
    if(!gLogFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        fprintf(stderr, "[log] open failed: %s\n", gLogFile.errorString().toLocal8Bit().constData());
        return;
    }
    gLogFile.write("=== session start ===\n");
    gLogFile.flush();
    fprintf(stderr, "[log] handler installed: %s\n", gLogBase.toLocal8Bit().constData());
    if(!qEnvironmentVariableIsEmpty("QIMGV_DEBUG"))
        // only our categories — a bare "*.debug = true" would also flood the
        // file with Qt-internal plugin scanning noise
        QLoggingCategory::setFilterRules("qimgv.perf.*.debug = true\n");
    gPrevHandler = qInstallMessageHandler(qimgvFileLog);
}

//------------------------------------------------------------------------------
void saveSettings() {
    delete settings;
}
//------------------------------------------------------------------------------
QDataStream& operator<<(QDataStream& out, const Script& v) {
    out << v.command << v.blocking;
    return out;
}
//------------------------------------------------------------------------------
QDataStream& operator>>(QDataStream& in, Script& v) {
    in >> v.command;
    in >> v.blocking;
    return in;
}
//------------------------------------------------------------------------------
int main(int argc, char *argv[]) {

    // force some env variables

#ifdef _WIN32
    // if this is set by other app, platform plugin may fail to load
    // https://github.com/easymodo/qimgv/issues/410
    qputenv("QT_PLUGIN_PATH","");
#endif

    // for hidpi testing
    //qputenv("QT_SCALE_FACTOR","1.5");
    //qputenv("QT_SCREEN_SCALE_FACTORS", "1;1.7");

    // do we still need this?
    qputenv("QT_AUTO_SCREEN_SCALE_FACTOR","0");

#if (QT_VERSION_MAJOR == 5)
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

    // Qt6 hidpi rendering on windows still has artifacts
    // This disables it for scale factors < 1.75
    // In this case only fonts are scaled
#ifdef _WIN32
#if (QT_VERSION_MAJOR == 6)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
#endif
#endif

    //qDebug() << qgetenv("QT_SCALE_FACTOR");
    //qDebug() << qgetenv("QT_SCREEN_SCALE_FACTORS");
    //qDebug() << qgetenv("QT_ENABLE_HIGHDPI_SCALING");

#ifdef __APPLE__
    MacOSApplication a(argc, argv);
    // default to "fusion" if available ("macos" has layout bugs, weird comboboxes etc)
    if(QStyleFactory::keys().contains("Fusion"))
        a.setStyle(QStyleFactory::create("Fusion"));
#else
    QApplication a(argc, argv);
    // use some style workarounds
    a.setStyle(new ProxyStyle);
#endif

    QCoreApplication::setOrganizationName("qimgv");
    QCoreApplication::setOrganizationDomain("github.com/easymodo/qimgv");
    QCoreApplication::setApplicationName("qimgv");
    QCoreApplication::setApplicationVersion(appVersion.toString());
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    QGuiApplication::setDesktopFileName(QCoreApplication::applicationName() + ".desktop");

    qDebug() << "[rustcore] qimgv-engine version:" << qimgv_engine_version();

    // needed for mpv
#ifndef _MSC_VER
    setlocale(LC_NUMERIC, "C");
#endif

#ifdef __GLIBC__
    // default value of 128k causes memory fragmentation issues
    mallopt(M_MMAP_THRESHOLD, 64000);
#endif

    // use custom types in signals
    qRegisterMetaType<ScalerRequest>("ScalerRequest");
    qRegisterMetaType<Script>("Script");
    qRegisterMetaType<std::shared_ptr<Image>>("std::shared_ptr<Image>");
    qRegisterMetaType<std::shared_ptr<Thumbnail>>("std::shared_ptr<Thumbnail>");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    qRegisterMetaTypeStreamOperators<Script>("Script");
#endif

    // globals
    inputMap = InputMap::getInstance();
    appActions = Actions::getInstance();
    settings = Settings::getInstance();
    scriptManager = ScriptManager::getInstance();
    actionManager = ActionManager::getInstance();
    shrRes = SharedResources::getInstance();

    atexit(saveSettings);
    setupFileLogging();
    qDebug() << "[probe] message right after install";

// parse args ------------------------------------------------------------------
    QCommandLineParser parser;
    QString appDescription = qApp->applicationName() + " - Fast and configurable image viewer.";
    appDescription.append("\nVersion: " + qApp->applicationVersion());
    appDescription.append("\nLicense: GNU GPLv3");
    parser.setApplicationDescription(appDescription);
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("path", QCoreApplication::translate("main", "File or directory path."));
    parser.addOptions({
        {"gen-thumbs",
            QCoreApplication::translate("main", "Generate all thumbnails for directory."),
            QCoreApplication::translate("main", "directory-path")},
        {"gen-thumbs-size",
            QCoreApplication::translate("main", "Thumbnail size. Current size is used if not specified."),
            QCoreApplication::translate("main", "thumbnail-size")},
        {"build-options",
            QCoreApplication::translate("main", "Show build options.")},
    });
    parser.process(a);

    if(parser.isSet("build-options")) {
        CmdOptionsRunner r;
        QTimer::singleShot(0, &r, &CmdOptionsRunner::showBuildOptions);
        return a.exec();
    } else if(parser.isSet("gen-thumbs")) {
        int size = settings->folderViewIconSize();
        if(parser.isSet("gen-thumbs-size"))
            size = parser.value("gen-thumbs-size").toInt();

        CmdOptionsRunner r;
        QTimer::singleShot(0, &r,
                           std::bind(&CmdOptionsRunner::generateThumbs, &r, parser.value("gen-thumbs"), size));
        return a.exec();
    }

// -----------------------------------------------------------------------------

    Core core;

#ifdef __APPLE__
    QObject::connect(&a, &MacOSApplication::fileOpened, &core, &Core::loadPath);
#endif

    if(parser.positionalArguments().count())
        core.loadPath(parser.positionalArguments().at(0));
    else if(settings->defaultViewMode() == MODE_FOLDERVIEW)
        core.loadPath(QDir::homePath());

    // wait for event queue to catch up before showing window
    // this avoids white background flicker on windows (or not?)
    qApp->processEvents();

    core.showGui();
    return a.exec();
}
