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
#include "utils/filelog.h"

#include "qimgv_engine.h"

#ifdef __APPLE__
#include "macosapplication.h"
#endif

//------------------------------------------------------------------------------
void saveSettings() {
    delete settings;
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

    // hidpi rendering on windows still has artifacts
    // This disables it for scale factors < 1.75
    // In this case only fonts are scaled
#ifdef _WIN32
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
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

    QCoreApplication::setOrganizationName("ImPulse");
    QCoreApplication::setOrganizationDomain("github.com/easymodo/qimgv");
    QCoreApplication::setApplicationName("impulse");
    QCoreApplication::setApplicationVersion(appVersion.toString());
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);

    // no ".desktop" suffix — Qt6 rejects it; the name is matched against the
    // icon theme and the session desktop either way
    QGuiApplication::setDesktopFileName(QCoreApplication::applicationName());
    QGuiApplication::setApplicationDisplayName("ImPulse");

    // the engine crate's version is frozen Cargo metadata and carries no
    // information; the release version is the app's own
    qDebug() << "[impulse] version:" << appVersion.toString();

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
    qRegisterMetaType<std::shared_ptr<Image>>("std::shared_ptr<Image>");
    qRegisterMetaType<std::shared_ptr<Thumbnail>>("std::shared_ptr<Thumbnail>");
    // the log handler goes up before anything reads config: startup-window
    // warnings (the QSettings @Variant load screams among them) previously
    // reached only stderr and never landed in the session log
    FileLog::install(Settings::resolveCacheDir() + "/logs");
    // globals
    inputMap = InputMap::getInstance();
    appActions = Actions::getInstance();
    settings = Settings::getInstance();
    scriptManager = ScriptManager::getInstance();
    actionManager = ActionManager::getInstance();
    shrRes = SharedResources::getInstance();

    atexit(saveSettings);
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
        {"build-options",
            QCoreApplication::translate("main", "Show build options.")},
    });
    parser.process(a);

    if(parser.isSet("build-options")) {
        CmdOptionsRunner r;
        QTimer::singleShot(0, &r, &CmdOptionsRunner::showBuildOptions);
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

    // a cold open armed to the listing landing owns the reveal: showing here
    // would flash an empty window before the first frame exists
    if(!core.firstFramePending())
        core.showGui();
    return a.exec();
}
