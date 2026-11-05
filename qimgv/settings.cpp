#include <type_traits>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include "settings.h"

Settings *settings = nullptr;

namespace {
// Qt6 removed the QDataStream channel that let custom types ride in QSettings
// as @Variant(...): writes land as empty shells with a "QVariant::save"
// warning, reads land with "unable to load type". Scripts persist as a JSON
// string in the same value slot instead.
QString scriptToJson(const Script &s) {
    QJsonObject obj;
    obj.insert(QStringLiteral("command"), s.command);
    obj.insert(QStringLiteral("blocking"), s.blocking);
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

Script scriptFromStoredValue(const QVariant &value) {
    Script s;
    if(value.typeId() != QMetaType::QString)
        return s;
    const QString raw = value.toString().trimmed();
    if(!raw.startsWith('{'))
        return s;
    QJsonParseError err;
    const QJsonObject obj = QJsonDocument::fromJson(raw.toUtf8(), &err).object();
    if(err.error != QJsonParseError::NoError)
        return s;
    s.command = obj.value(QStringLiteral("command")).toString();
    s.blocking = obj.value(QStringLiteral("blocking")).toBool();
    return s;
}
template<class T> constexpr SettingDesc::Kind kindOf();
template<> constexpr SettingDesc::Kind kindOf<bool>()      { return SettingDesc::Kind::Bool; }
template<> constexpr SettingDesc::Kind kindOf<int>()        { return SettingDesc::Kind::Int; }
template<> constexpr SettingDesc::Kind kindOf<qlonglong>()  { return SettingDesc::Kind::Int; }
template<> constexpr SettingDesc::Kind kindOf<float>()      { return SettingDesc::Kind::Real; }
template<> constexpr SettingDesc::Kind kindOf<double>()     { return SettingDesc::Kind::Real; }
template<> constexpr SettingDesc::Kind kindOf<QString>()    { return SettingDesc::Kind::Str; }

template<class T> T settingDefault(const SettingDesc &d) {
    Q_ASSERT(d.kind == kindOf<T>());
    if constexpr(std::is_same_v<T, bool>)
        return d.defB;
    else if constexpr(std::is_same_v<T, int> || std::is_same_v<T, qlonglong>)
        return static_cast<T>(d.defI);
    else if constexpr(std::is_same_v<T, float> || std::is_same_v<T, double>)
        return static_cast<T>(d.defR);
    else
        return QString::fromUtf8(d.defS);
}
}

template<class T>
T Settings::load(const SettingDesc &d) const {
    Q_ASSERT(d.kind == kindOf<T>());
    const QVariant raw = settingsConf->value(d.key);
    if(!raw.isValid())
        return settingDefault<T>(d);
    if constexpr(std::is_same_v<T, bool>) {
        return raw.toBool();
    } else if constexpr(std::is_same_v<T, int> || std::is_same_v<T, qlonglong>) {
        // QVariant::canConvert is lenient for QString->number (junk "converts"
        // to 0), so the ok flag decides — INI configs hand values back as
        // strings, and "3000" must convert while "banana" must not
        bool ok = false;
        qlonglong v = raw.toLongLong(&ok);
        if(!ok)
            return settingDefault<T>(d);
        if(d.hasLo) v = std::max<qlonglong>(v, static_cast<qlonglong>(d.lo));
        if(d.hasHi) v = std::min<qlonglong>(v, static_cast<qlonglong>(d.hi));
        return static_cast<T>(v);
    } else if constexpr(std::is_same_v<T, float> || std::is_same_v<T, double>) {
        bool ok = false;
        double v = raw.toDouble(&ok);
        if(!ok)
            return settingDefault<T>(d);
        if(d.hasLo) v = std::max(v, d.lo);
        if(d.hasHi) v = std::min(v, d.hi);
        return static_cast<T>(v);
    } else {
        return raw.toString();
    }
}

template<class T>
void Settings::store(const SettingDesc &d, const T &v) {
    Q_ASSERT(d.kind == kindOf<T>());
    settingsConf->setValue(d.key, v);
}

Settings::Settings(QObject *parent) : QObject(parent) {
#if defined(__linux__) || defined(__FreeBSD__)
    // config files
    QSettings::setDefaultFormat(QSettings::NativeFormat);
    settingsConf = new QSettings();
    stateConf = new QSettings(QCoreApplication::organizationName(), "savedState");
    themeConf = new QSettings(QCoreApplication::organizationName(), "theme");
#else
    mConfDir = new QDir(QApplication::applicationDirPath() + "/conf");
    mConfDir->mkpath(QApplication::applicationDirPath() + "/conf");
    settingsConf = new QSettings(mConfDir->absolutePath() + "/" + qApp->applicationName() + ".ini", QSettings::IniFormat);
    stateConf = new QSettings(mConfDir->absolutePath() + "/savedState.ini", QSettings::IniFormat);
    themeConf = new QSettings(mConfDir->absolutePath() + "/theme.ini", QSettings::IniFormat);
#endif
    fillVideoFormats();
    // cache paths live in the constructor, not in an external protocol: a
    // directly-constructed instance (tests) used to carry uninitialized
    // directory pointers and segfault in its own destructor
    setupCache();
}
//------------------------------------------------------------------------------
Settings::~Settings() {
    saveTheme();
    delete mThumbCacheDir;
    delete mTmpDir;
    delete settingsConf;
    delete stateConf;
    delete themeConf;
}
//------------------------------------------------------------------------------
Settings *Settings::getInstance() {
    if(!settings) {
        settings = new Settings();
        settings->loadTheme();
    }
    return settings;
}
//------------------------------------------------------------------------------
QString Settings::resolveCacheDir() {
#if defined(__linux__) || defined(__FreeBSD__)
    QString genericCacheLocation = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if(genericCacheLocation.isEmpty())
        genericCacheLocation = QDir::homePath() + "/.cache";
    genericCacheLocation.append("/" + QApplication::applicationName());
    QString cacheLocation = QSettings().value("cacheDir", genericCacheLocation).toString();
    QDir(cacheLocation).mkpath(cacheLocation);
    QFileInfo dirTest(cacheLocation);
    if(!dirTest.isDir() || !dirTest.isWritable() || !dirTest.exists()) {
        // fallback
        qDebug() << "Error: cache dir is not writable" << cacheLocation;
        qDebug() << "Trying to use" << genericCacheLocation << "instead";
        QDir(genericCacheLocation).mkpath(genericCacheLocation);
        cacheLocation = genericCacheLocation;
    }
    return cacheLocation;
#else
    QString cacheLocation = QApplication::applicationDirPath() + "/cache";
    QDir(cacheLocation).mkpath(cacheLocation);
    return cacheLocation;
#endif
}
//------------------------------------------------------------------------------
void Settings::setupCache() {
    mTmpDir = new QDir(resolveCacheDir());
    mThumbCacheDir = new QDir(mTmpDir->absolutePath() + "/thumbnails");
    mThumbCacheDir->mkpath(mThumbCacheDir->absolutePath());
}
//------------------------------------------------------------------------------
void Settings::sync() {
    settingsConf->sync();
    stateConf->sync();
}
//------------------------------------------------------------------------------
QString Settings::thumbnailCacheDir() {
    return mThumbCacheDir->path() + "/";
}
//------------------------------------------------------------------------------
QString Settings::tmpDir() {
    return mTmpDir->path() + "/";
}
//------------------------------------------------------------------------------
// this here is temporarily, will be moved to some sort of theme manager class
void Settings::loadStylesheet() {
    // stylesheet template file
    QFile file(":/res/styles/style-template.qss");
    if(file.open(QFile::ReadOnly)) {
        QString styleSheet = QLatin1String(file.readAll());

        // --- color scheme ---------------------------------------------
        auto colors = colorScheme();
        // tint color for system windows
        QPalette p;
        QColor sys_text = p.text().color();
        QColor sys_window = p.window().color();
        QColor sys_window_tinted, sys_window_tinted_lc, sys_window_tinted_lc2, sys_window_tinted_hc, sys_window_tinted_hc2;
        if(sys_window.valueF() <= 0.45f) {
            // dark system theme
            sys_window_tinted_lc2.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() + 6);
            sys_window_tinted_lc.setHsv(sys_window.hue(),  sys_window.saturation(), sys_window.value() + 14);
            sys_window_tinted.setHsv(sys_window.hue(),     sys_window.saturation(), sys_window.value() + 20);
            sys_window_tinted_hc.setHsv(sys_window.hue(),  sys_window.saturation(), sys_window.value() + 35);
            sys_window_tinted_hc2.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() + 50);
        } else {
            // light system theme
            sys_window_tinted_lc2.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() - 6);
            sys_window_tinted_lc.setHsv(sys_window.hue(),  sys_window.saturation(), sys_window.value() - 14);
            sys_window_tinted.setHsv(sys_window.hue(),     sys_window.saturation(), sys_window.value() - 20);
            sys_window_tinted_hc.setHsv(sys_window.hue(),  sys_window.saturation(), sys_window.value() - 35);
            sys_window_tinted_hc2.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() - 50);
        }

        // --- widget sizes ---------------------------------------------
        auto fnt = QGuiApplication::font();
        QFontMetrics fm(fnt);
        // todo: use precise values for ~9-11 point sizes
        int font_small = qMax(static_cast<int>(fnt.pointSize() * 0.9f), 8);
        int font_large = static_cast<int>(fnt.pointSize() * 1.8f);
        int text_height = fm.height();
        int text_padding = static_cast<int>(text_height * 0.10f);
        int text_padding_large = static_cast<int>(text_height * 0.25f);

        // folderview top panel item sizes
        int top_panel_v_margin = 4;
        // ensure at least 4px so its not too thin
        int top_panel_text_padding = qMax(text_padding, 4);
        // scale with font, 38px base size
        int top_panel_height = qMax((text_height + top_panel_text_padding * 2 + top_panel_v_margin * 2), 38);

        // overlay headers
        // 32px base size
        int overlay_header_size = qMax(text_height + text_padding * 2, 30);

        // todo
        int button_height = text_height + text_padding_large * 2;

        // pseudo-dpi to scale some widget widths
        int text_height_base = 22;
        qreal pDpr = qMax( (static_cast<qreal>(text_height) / text_height_base), 1.0);
        int context_menu_width = 212 * pDpr;
        int context_menu_button_height = 32 * pDpr;
        int rename_overlay_width = 380 * pDpr;

        //qDebug()<< "dpr=" << qApp->devicePixelRatio() << "pDpr=" << pDpr;

        // --- write variables into stylesheet --------------------------
        styleSheet.replace("%font_small%", QString::number(font_small)+"pt");
        styleSheet.replace("%font_large%", QString::number(font_large)+"pt");
        styleSheet.replace("%button_height%", QString::number(button_height)+"px");
        styleSheet.replace("%top_panel_height%", QString::number(top_panel_height)+"px");
        styleSheet.replace("%overlay_header_size%", QString::number(overlay_header_size)+"px");
        styleSheet.replace("%context_menu_width%", QString::number(context_menu_width)+"px");
        styleSheet.replace("%context_menu_button_height%", QString::number(context_menu_button_height)+"px");
        styleSheet.replace("%rename_overlay_width%", QString::number(rename_overlay_width)+"px");

        styleSheet.replace("%icontheme%",  "light");
        // Qt::Popup can't do transparency under windows, use square window
#ifdef _WIN32
        styleSheet.replace("%contextmenu_border_radius%",  "0px");
#else
        styleSheet.replace("%contextmenu_border_radius%",  "3px");
#endif
        styleSheet.replace("%sys_window%",    sys_window.name());
        styleSheet.replace("%sys_window_tinted%",    sys_window_tinted.name());
        styleSheet.replace("%sys_window_tinted_lc%", sys_window_tinted_lc.name());
        styleSheet.replace("%sys_window_tinted_lc2%", sys_window_tinted_lc2.name());
        styleSheet.replace("%sys_window_tinted_hc%", sys_window_tinted_hc.name());
        styleSheet.replace("%sys_window_tinted_hc2%", sys_window_tinted_hc2.name());
        styleSheet.replace("%sys_text_secondary_rgba%", "rgba(" + QString::number(sys_text.red())   + ","
                                                      + QString::number(sys_text.green()) + ","
                                                      + QString::number(sys_text.blue())  + ",50%)");

        styleSheet.replace("%button%",               colors.button.name());
        styleSheet.replace("%button_hover%",         colors.button_hover.name());
        styleSheet.replace("%button_pressed%",       colors.button_pressed.name());
        styleSheet.replace("%panel_button%",         colors.panel_button.name());
        styleSheet.replace("%panel_button_hover%",   colors.panel_button_hover.name());
        styleSheet.replace("%panel_button_pressed%", colors.panel_button_pressed.name());
        styleSheet.replace("%widget%",               colors.widget.name());
        styleSheet.replace("%widget_border%",        colors.widget_border.name());
        styleSheet.replace("%folderview%",           colors.folderview.name());
        styleSheet.replace("%folderview_topbar%",    colors.folderview_topbar.name());
        styleSheet.replace("%folderview_hc%",        colors.folderview_hc.name());
        styleSheet.replace("%folderview_hc2%",       colors.folderview_hc2.name());
        styleSheet.replace("%accent%",               colors.accent.name());
        styleSheet.replace("%input_field_focus%",    colors.input_field_focus.name());
        styleSheet.replace("%overlay%",              colors.overlay.name());
        styleSheet.replace("%icons%",                colors.icons.name());
        styleSheet.replace("%text_hc2%",             colors.text_hc2.name());
        styleSheet.replace("%text_hc%",              colors.text_hc.name());
        styleSheet.replace("%text%",                 colors.text.name());
        styleSheet.replace("%overlay_text%",         colors.overlay_text.name());
        styleSheet.replace("%text_lc%",              colors.text_lc.name());
        styleSheet.replace("%text_lc2%",             colors.text_lc2.name());
        styleSheet.replace("%scrollbar%",            colors.scrollbar.name());
        styleSheet.replace("%scrollbar_hover%",      colors.scrollbar_hover.name());
        styleSheet.replace("%folderview_button_hover%",   colors.folderview_button_hover.name());
        styleSheet.replace("%folderview_button_pressed%", colors.folderview_button_pressed.name());
        styleSheet.replace("%text_secondary_rgba%",  "rgba(" + QString::number(colors.text.red())   + ","
                                                             + QString::number(colors.text.green()) + ","
                                                             + QString::number(colors.text.blue())  + ",62%)");
        styleSheet.replace("%accent_hover_rgba%",    "rgba(" + QString::number(colors.accent.red())   + ","
                                                             + QString::number(colors.accent.green()) + ","
                                                             + QString::number(colors.accent.blue())  + ",65%)");
        styleSheet.replace("%overlay_rgba%",         "rgba(" + QString::number(colors.overlay.red())   + ","
                                                             + QString::number(colors.overlay.green()) + ","
                                                             + QString::number(colors.overlay.blue())  + ",90%)");
        styleSheet.replace("%fv_backdrop_rgba%",     "rgba(" + QString::number(colors.folderview_hc2.red())   + ","
                                                             + QString::number(colors.folderview_hc2.green()) + ","
                                                             + QString::number(colors.folderview_hc2.blue())  + ",80%)");
        // do not show separator line if topbar color matches folderview
        if(colors.folderview != colors.folderview_topbar)
            styleSheet.replace("%topbar_border_rgba%", "rgba(0,0,0,14%)");
        else
            styleSheet.replace("%topbar_border_rgba%", colors.folderview.name());

        // --- apply -------------------------------------------------
        qApp->setStyleSheet(styleSheet);
    }
}
//------------------------------------------------------------------------------
void Settings::loadTheme() {
    if(useSystemColorScheme()) {
        setColorScheme(ThemeStore::colorScheme(ColorSchemes::COLORS_SYSTEM));
    } else {
        BaseColorScheme base;
        themeConf->beginGroup("Colors");
    for(const ThemeColorDesc &row : kThemeColors) {
        QColor c(themeConf->value(row.key, row.def).toString());
        if(!c.isValid())
            c = QColor(row.def); // junk from a hand-edited theme file
        base.*row.base = c;
    }
    base.tid = themeConf->value("tid", "-1").toInt();
        themeConf->endGroup();
        setColorScheme(ColorScheme(base));
    }
}
void Settings::saveTheme() {
    if(useSystemColorScheme())
        return;
    themeConf->beginGroup("Colors");
    for(const ThemeColorDesc &row : kThemeColors)
        themeConf->setValue(row.key, (mColorScheme.*row.scheme).name());
    themeConf->setValue("tid", mColorScheme.tid);
    themeConf->endGroup();
}
//------------------------------------------------------------------------------
const ColorScheme& Settings::colorScheme() {
    return mColorScheme;
}
//------------------------------------------------------------------------------
void Settings::setColorScheme(ColorScheme scheme) {
    mColorScheme = scheme;
    loadStylesheet();
}
//------------------------------------------------------------------------------
void Settings::setColorTid(int tid) {
    mColorScheme.tid = tid;
}
//------------------------------------------------------------------------------
void Settings::fillVideoFormats() {
    mVideoFormatsMap.insert("video/webm",       "webm");
    mVideoFormatsMap.insert("video/mp4",        "mp4");
    mVideoFormatsMap.insert("video/mp4",        "m4v");
    mVideoFormatsMap.insert("video/mpeg",       "mpg");
    mVideoFormatsMap.insert("video/mpeg",       "mpeg");
    mVideoFormatsMap.insert("video/x-matroska", "mkv");
    mVideoFormatsMap.insert("video/x-ms-wmv",   "wmv");
    mVideoFormatsMap.insert("video/x-msvideo",  "avi");
    mVideoFormatsMap.insert("video/quicktime",  "mov");
    mVideoFormatsMap.insert("video/x-flv",      "flv");
}
//------------------------------------------------------------------------------
QString Settings::mpvBinary() {
    QString mpvPath = settingsConf->value("mpvBinary", "").toString();
    if(!QFile::exists(mpvPath)) {
    #ifdef _WIN32
        mpvPath = QCoreApplication::applicationDirPath() + "/mpv.exe";
    #elif defined __linux__
        mpvPath = "/usr/bin/mpv";
    #elif defined __FreeBSD__
        mpvPath = "/usr/local/bin/mpv";
    #endif
        if(!QFile::exists(mpvPath))
            mpvPath = "";
    }
    return mpvPath;
}

void Settings::setMpvBinary(const QString& path) {
    if(QFile::exists(path)) {
        store(K_MpvBinary, path);
    }
}
//------------------------------------------------------------------------------
QList<QByteArray> Settings::supportedFormats() {
    auto formats = QImageReader::supportedImageFormats();
    formats << "jfif";
    if(videoPlayback())
        formats << mVideoFormatsMap.values();
    formats.removeAll("pdf");
    return formats;
}
//------------------------------------------------------------------------------
// (for open/save dialogs, as a single string)
// example:  "Images (*.jpg, *.png)"
QString Settings::supportedFormatsFilter() {
    QString filters;
    auto formats = supportedFormats();
    filters.append("Supported files (");
    for(int i = 0; i < formats.count(); i++)
        filters.append("*." + QString(formats.at(i)) + " ");
    filters.append(")");
    return filters;
}
//------------------------------------------------------------------------------
QString Settings::supportedFormatsRegex() {
    QString filter;
    QList<QByteArray> formats = supportedFormats();
    filter.append(".*\\.(");
    for(int i = 0; i < formats.count(); i++)
        filter.append(QString(formats.at(i)) + "|");
    filter.chop(1);
    filter.append(")$");
    return filter;
}
//------------------------------------------------------------------------------
// returns list of mime types
QStringList Settings::supportedMimeTypes() {
    QStringList filters;
    QList<QByteArray> mimeTypes = QImageReader::supportedMimeTypes();
    if(videoPlayback())
        mimeTypes << mVideoFormatsMap.keys();
    for(int i = 0; i < mimeTypes.count(); i++) {
        filters << QString(mimeTypes.at(i));
    }
    return filters;
}
//------------------------------------------------------------------------------
bool Settings::videoPlayback() {
#ifdef USE_MPV
    return load<bool>(K_VideoPlayback);
#else
    return false;
#endif
}

void Settings::setVideoPlayback(bool mode) {
    store(K_VideoPlayback, mode);
}
//------------------------------------------------------------------------------
bool Settings::useSystemColorScheme() {
    return load<bool>(K_UseSystemColorScheme);
}

void Settings::setUseSystemColorScheme(bool mode) {
    store(K_UseSystemColorScheme, mode);
}
//------------------------------------------------------------------------------
QVersionNumber Settings::lastVersion() {
    int vmajor = settingsConf->value("lastVerMajor", 0).toInt();
    int vminor = settingsConf->value("lastVerMinor", 0).toInt();
    int vmicro = settingsConf->value("lastVerMicro", 0).toInt();
    return QVersionNumber(vmajor, vminor, vmicro);
}

void Settings::setLastVersion(QVersionNumber &ver) {
    settingsConf->setValue("lastVerMajor", ver.majorVersion());
    settingsConf->setValue("lastVerMinor", ver.minorVersion());
    settingsConf->setValue("lastVerMicro", ver.microVersion());
}
//------------------------------------------------------------------------------
void Settings::setShowChangelogs(bool mode) {
    store(K_ShowChangelogs, mode);
}

bool Settings::showChangelogs() {
    return load<bool>(K_ShowChangelogs);
}
//------------------------------------------------------------------------------
qreal Settings::backgroundOpacity() {
    bool ok = false;
    qreal value = settingsConf->value("backgroundOpacity", 1.0).toReal(&ok);
    if(!ok)
        return 0.0;
    if(value > 1.0)
        return 1.0;
    if(value < 0.0)
        return 0.0;
    return value;
}

void Settings::setBackgroundOpacity(qreal value) {
    if(value > 1.0)
        value = 1.0;
    else if(value < 0.0)
        value = 0.0;
    store(K_BackgroundOpacity, value);
}
//------------------------------------------------------------------------------
bool Settings::blurBackground() {
#ifndef USE_KDE_BLUR
    return false;
#endif
    return load<bool>(K_BlurBackground);
}

void Settings::setBlurBackground(bool mode) {
    store(K_BlurBackground, mode);
}
//------------------------------------------------------------------------------
void Settings::setSortingMode(SortingMode mode) {
    if(mode >= 6)
        mode = SortingMode::SORT_NAME;
    store(K_SortingMode, static_cast<qlonglong>(mode));
}

SortingMode Settings::sortingMode() {
    int mode = settingsConf->value("sortingMode", 0).toInt();
    if(mode < 0 || mode >= 6)
        mode = 0;
    return static_cast<SortingMode>(mode);
}
//------------------------------------------------------------------------------
bool Settings::playVideoSounds() {
    return load<bool>(K_PlayVideoSounds);
}

void Settings::setPlayVideoSounds(bool mode) {
    store(K_PlayVideoSounds, mode);
}
//------------------------------------------------------------------------------
void Settings::setVolume(int vol) {
    stateConf->setValue("volume", vol);
}

int Settings::volume() {
    return stateConf->value("volume", 100).toInt();
}
//------------------------------------------------------------------------------
FolderViewMode Settings::folderViewMode() {
    int mode = settingsConf->value("folderViewMode", 2).toInt();
    if(mode < 0 || mode >= 3)
        mode = 2;
    return static_cast<FolderViewMode>(mode);
}

void Settings::setFolderViewMode(FolderViewMode mode) {
    store(K_FolderViewMode, static_cast<qlonglong>(mode));
}
//------------------------------------------------------------------------------
ThumbPanelStyle Settings::thumbPanelStyle() {
    int mode = settingsConf->value("thumbPanelStyle", 1).toInt();
    if(mode < 0 || mode > 1)
        mode = 1;
    return static_cast<ThumbPanelStyle>(mode);
}

void Settings::setThumbPanelStyle(ThumbPanelStyle mode) {
    store(K_ThumbPanelStyle, static_cast<qlonglong>(mode));
}
//------------------------------------------------------------------------------
const QMultiMap<QByteArray, QByteArray> Settings::videoFormats() const {
    return mVideoFormatsMap;
}
//------------------------------------------------------------------------------
int Settings::panelPreviewsSize() {
    bool ok = true;
    int size = settingsConf->value("panelPreviewsSize", 140).toInt(&ok);
    if(!ok)
        size = 140;
    size = qBound(100, size, 250);
    return size;
}

void Settings::setPanelPreviewsSize(int size) {
    store(K_PanelPreviewsSize, size);
}
//------------------------------------------------------------------------------
bool Settings::usePreloader() {
    return load<bool>(K_UsePreloader);
}

void Settings::setUsePreloader(bool mode) {
    store(K_UsePreloader, mode);
}
//------------------------------------------------------------------------------
bool Settings::keepFitMode() {
    return load<bool>(K_KeepFitMode);
}

void Settings::setKeepFitMode(bool mode) {
    store(K_KeepFitMode, mode);
}
//------------------------------------------------------------------------------
bool Settings::fullscreenMode() {
    return load<bool>(K_FullscreenMode);
}

void Settings::setFullscreenMode(bool mode) {
    store(K_FullscreenMode, mode);
}
//------------------------------------------------------------------------------
bool Settings::maximizedWindow() {
    return stateConf->value("maximizedWindow", false).toBool();
}

void Settings::setMaximizedWindow(bool mode) {
    stateConf->setValue("maximizedWindow", mode);
}
//------------------------------------------------------------------------------
bool Settings::panelEnabled() {
    return load<bool>(K_PanelEnabled);
}

void Settings::setPanelEnabled(bool mode) {
    store(K_PanelEnabled, mode);
}
//------------------------------------------------------------------------------
bool Settings::panelFullscreenOnly() {
    return load<bool>(K_PanelFullscreenOnly);
}

void Settings::setPanelFullscreenOnly(bool mode) {
    store(K_PanelFullscreenOnly, mode);
}
//------------------------------------------------------------------------------
int Settings::lastDisplay() {
    return stateConf->value("lastDisplay", 0).toInt();
}

void Settings::setLastDisplay(int display) {
    stateConf->setValue("lastDisplay", display);
}
//------------------------------------------------------------------------------
PanelPosition Settings::panelPosition() {
    QString posString = settingsConf->value("panelPosition", "top").toString();
    if(posString == "top") {
        return PanelPosition::PANEL_TOP;
    } else if(posString == "bottom") {
        return PanelPosition::PANEL_BOTTOM;
    } else if(posString == "left") {
        return PanelPosition::PANEL_LEFT;
    } else {
        return PanelPosition::PANEL_RIGHT;
    }
}

void Settings::setPanelPosition(PanelPosition pos) {
    QString posString;
    switch(pos) {
        case PANEL_TOP:
            posString = "top";
            break;
        case PANEL_BOTTOM:
            posString = "bottom";
            break;
        case PANEL_LEFT:
            posString = "left";
            break;
        case PANEL_RIGHT:
            posString = "right";
            break;
    }
    store(K_PanelPosition, posString);
}
//------------------------------------------------------------------------------
bool Settings::panelPinned() {
    return load<bool>(K_PanelPinned);
}

void Settings::setPanelPinned(bool mode) {
    store(K_PanelPinned, mode);
}
//------------------------------------------------------------------------------
/*
 * 0: fit window
 * 1: fit width
 * 2: orginal size
 * 3: fit window (stretch)
 */
ImageFitMode Settings::imageFitMode() {
    int mode = settingsConf->value("defaultFitMode", 0).toInt();
    if(mode < 0 || mode > 3) {
        qDebug() << "Settings: Invalid fit mode ( " + QString::number(mode) + " ). Resetting to default.";
        mode = 0;
    }
    return static_cast<ImageFitMode>(mode);
}

void Settings::setImageFitMode(ImageFitMode mode) {
    int modeInt = static_cast<ImageFitMode>(mode);
    if(modeInt < 0 || modeInt > 3) {
        qDebug() << "Settings: Invalid fit mode ( " + QString::number(modeInt) + " ). Resetting to default.";
        modeInt = 0;
    }
    store(K_DefaultFitMode, modeInt);
}
//------------------------------------------------------------------------------
QRect Settings::windowGeometry() {
    QRect savedRect = stateConf->value("windowGeometry").toRect();
    if(savedRect.size().isEmpty())
        savedRect.setRect(100, 100, 900, 600);
    return savedRect;
}

void Settings::setWindowGeometry(QRect geometry) {
    stateConf->setValue("windowGeometry", geometry);
}
//------------------------------------------------------------------------------
bool Settings::loopSlideshow() {
    return load<bool>(K_LoopSlideshow);
}

void Settings::setLoopSlideshow(bool mode) {
    store(K_LoopSlideshow, mode);
}
//------------------------------------------------------------------------------
void Settings::sendChangeNotification() {
    emit settingsChanged();
}
//------------------------------------------------------------------------------
void Settings::readShortcuts(QMap<QString, QString> &shortcuts) {
    settingsConf->beginGroup("Controls");
    QStringList in, pair;
    in = settingsConf->value("shortcuts").toStringList();
    for(int i = 0; i < in.count(); i++) {
        pair = in[i].split("=");
        if(!pair[0].isEmpty() && !pair[1].isEmpty()) {
            if(pair[1].endsWith("eq"))
                pair[1]=pair[1].chopped(2) + "=";
            shortcuts.insert(pair[1], pair[0]);
        }
    }
    settingsConf->endGroup();
}

void Settings::saveShortcuts(const QMap<QString, QString> &shortcuts) {
    settingsConf->beginGroup("Controls");
    QMapIterator<QString, QString> i(shortcuts);
    QStringList out;
    while(i.hasNext()) {
        i.next();
        if(i.key().endsWith("="))
            out << i.value() + "=" + i.key().chopped(1) + "eq";
        else
            out << i.value() + "=" + i.key();
    }
    settingsConf->setValue("shortcuts", out);
    settingsConf->endGroup();
}
//------------------------------------------------------------------------------
void Settings::readScripts(QMap<QString, Script> &scripts) {
    scripts.clear();
    settingsConf->beginGroup("Scripts");

    // pre-@Variant generation layout: <n>/name and <n>/command sit directly
    // under the group; collect them so the array pass can fall back to real
    // data when the value slot holds a Qt6-dead shell
    QMap<int, Script> legacy;
    const QStringList groupKeys = settingsConf->allKeys();
    for(const QString &k : groupKeys) {
        const QStringList parts = k.split('/');
        if(parts.size() != 2 || parts.at(1) != QStringLiteral("command"))
            continue;
        bool isIndex = false;
        const int idx = parts.at(0).toInt(&isIndex);
        if(!isIndex)
            continue;
        Script s;
        s.command = settingsConf->value(k).toString();
        s.blocking = settingsConf->value(parts.at(0) + "/blocking").toBool();
        legacy.insert(idx, s);
    }

    int size = settingsConf->beginReadArray("script");
    for(int i=0; i < size; i++) {
        settingsConf->setArrayIndex(i);
        QString name = settingsConf->value("name").toString();
        Script scr = scriptFromStoredValue(settingsConf->value("value"));
        if(scr.command.isEmpty() && legacy.contains(i + 1))
            scr = legacy.value(i + 1);
        if(!name.isEmpty())
            scripts.insert(name, scr);
    }
    settingsConf->endArray();

    // legacy-only configs: group keys without any array at all
    if(size == 0) {
        for(auto it = legacy.constBegin(); it != legacy.constEnd(); ++it) {
            const QString name = settingsConf->value(QString::number(it.key()) + "/name").toString();
            if(!name.isEmpty())
                scripts.insert(name, it.value());
        }
    }
    settingsConf->endGroup();
}

void Settings::saveScripts(const QMap<QString, Script> &scripts) {
    settingsConf->beginGroup("Scripts");
    settingsConf->beginWriteArray("script");
    QMapIterator<QString, Script> i(scripts);
    int counter = 0;
    while(i.hasNext()) {
        i.next();
        settingsConf->setArrayIndex(counter);
        settingsConf->setValue("name", i.key());
        settingsConf->setValue("value", scriptToJson(i.value()));
        counter++;
    }
    settingsConf->endArray();
    settingsConf->endGroup();
}
//------------------------------------------------------------------------------
bool Settings::squareThumbnails() {
    return load<bool>(K_SquareThumbnails);
}

void Settings::setSquareThumbnails(bool mode) {
    store(K_SquareThumbnails, mode);
}
//------------------------------------------------------------------------------
bool Settings::transparencyGrid() {
    return load<bool>(K_TransparencyGrid);
}

void Settings::setTransparencyGrid(bool mode) {
    store(K_TransparencyGrid, mode);
}
//------------------------------------------------------------------------------
bool Settings::enableSmoothScroll() {
    return load<bool>(K_EnableSmoothScroll);
}

void Settings::setEnableSmoothScroll(bool mode) {
    store(K_EnableSmoothScroll, mode);
}
//------------------------------------------------------------------------------
bool Settings::useThumbnailCache() {
    return load<bool>(K_UseThumbnailCache);
}

void Settings::setUseThumbnailCache(bool mode) {
    store(K_UseThumbnailCache, mode);
}
//------------------------------------------------------------------------------
QStringList Settings::savedPaths() {
    return stateConf->value("savedPaths", QDir::homePath()).toStringList();
}

void Settings::setSavedPaths(const QStringList& paths) {
    stateConf->setValue("savedPaths", paths);
}
//------------------------------------------------------------------------------
QStringList Settings::bookmarks() {
    return stateConf->value("bookmarks").toStringList();
}

void Settings::setBookmarks(const QStringList& paths) {
    stateConf->setValue("bookmarks", paths);
}
//------------------------------------------------------------------------------
bool Settings::placesPanel() {
    return stateConf->value("placesPanel", true).toBool();
}

void Settings::setPlacesPanel(bool mode) {
    stateConf->setValue("placesPanel", mode);
}
//------------------------------------------------------------------------------
bool Settings::placesPanelBookmarksExpanded() {
    return stateConf->value("placesPanelBookmarksExpanded", true).toBool();
}

void Settings::setPlacesPanelBookmarksExpanded(bool mode) {
    stateConf->setValue("placesPanelBookmarksExpanded", mode);
}
//------------------------------------------------------------------------------
bool Settings::placesPanelTreeExpanded() {
    return stateConf->value("placesPanelTreeExpanded", true).toBool();
}

void Settings::setPlacesPanelTreeExpanded(bool mode) {
    stateConf->setValue("placesPanelTreeExpanded", mode);
}
//------------------------------------------------------------------------------
int Settings::placesPanelWidth() {
    return stateConf->value("placesPanelWidth", 260).toInt();
}

void Settings::setPlacesPanelWidth(int width) {
    stateConf->setValue("placesPanelWidth", width);
}
//------------------------------------------------------------------------------
void Settings::setSlideshowInterval(int ms) {
    store(K_SlideshowInterval, ms);
}

int Settings::slideshowInterval() {
    int interval = settingsConf->value("slideshowInterval", 3000).toInt();
    if(interval <= 0)
        interval = 3000;
    return interval;
}
//------------------------------------------------------------------------------
int Settings::thumbnailerThreadCount() {
    int count = settingsConf->value("thumbnailerThreads", 4).toInt();
    if(count < 1)
        count = 4;
    return count;
}

void Settings::setThumbnailerThreadCount(int count) {
    store(K_ThumbnailerThreads, count);
}
//------------------------------------------------------------------------------
bool Settings::smoothUpscaling() {
    return load<bool>(K_SmoothUpscaling);
}

void Settings::setSmoothUpscaling(bool mode) {
    store(K_SmoothUpscaling, mode);
}
//------------------------------------------------------------------------------
int Settings::folderViewIconSize() {
    return static_cast<int>(load<qlonglong>(K_FolderViewIconSize));
}

void Settings::setFolderViewIconSize(int value) {
    store(K_FolderViewIconSize, value);
}
//------------------------------------------------------------------------------
bool Settings::expandImage() {
    return load<bool>(K_ExpandImage);
}

void Settings::setExpandImage(bool mode) {
    store(K_ExpandImage, mode);
}
//------------------------------------------------------------------------------
int Settings::expandLimit() {
    return static_cast<int>(load<qlonglong>(K_ExpandLimit));
}

void Settings::setExpandLimit(int value) {
    store(K_ExpandLimit, value);
}
//------------------------------------------------------------------------------
int Settings::JPEGSaveQuality() {
    int quality = std::clamp(settingsConf->value("JPEGSaveQuality", 95).toInt(), 0, 100);
    return quality;
}

void Settings::setJPEGSaveQuality(int value) {
    store(K_JPEGSaveQuality, value);
}
//------------------------------------------------------------------------------
ScalingFilter Settings::scalingFilter() {
    int defaultFilter = 1;
#ifdef USE_OPENCV
    // default to a nicer QI_FILTER_CV_CUBIC
    defaultFilter = 3;
#endif
    int mode = settingsConf->value("scalingFilter", defaultFilter).toInt();
#ifndef USE_OPENCV
    if(mode > 2)
        mode = 1;
#endif
    if(mode < 0 || mode > 4)
        mode = 1;
    return static_cast<ScalingFilter>(mode);
}

void Settings::setScalingFilter(ScalingFilter mode) {
    settingsConf->setValue("scalingFilter", mode);
}
//------------------------------------------------------------------------------
bool Settings::smoothAnimatedImages() {
    return load<bool>(K_SmoothAnimatedImages);
}

void Settings::setSmoothAnimatedImages(bool mode) {
    store(K_SmoothAnimatedImages, mode);
}
//------------------------------------------------------------------------------
bool Settings::infoBarFullscreen() {
    return load<bool>(K_InfoBarFullscreen);
}

void Settings::setInfoBarFullscreen(bool mode) {
    store(K_InfoBarFullscreen, mode);
}
//------------------------------------------------------------------------------
bool Settings::infoBarWindowed() {
    return load<bool>(K_InfoBarWindowed);
}

void Settings::setInfoBarWindowed(bool mode) {
    store(K_InfoBarWindowed, mode);
}
//------------------------------------------------------------------------------
bool Settings::windowTitleExtendedInfo() {
    return load<bool>(K_WindowTitleExtendedInfo);
}

void Settings::setWindowTitleExtendedInfo(bool mode) {
    store(K_WindowTitleExtendedInfo, mode);
}

//------------------------------------------------------------------------------
bool Settings::cursorAutohide() {
    return load<bool>(K_CursorAutohide);
}

void Settings::setCursorAutohide(bool mode) {
    store(K_CursorAutohide, mode);
}
//------------------------------------------------------------------------------
bool Settings::firstRun() {
    return load<bool>(K_FirstRun);
}

void Settings::setFirstRun(bool mode) {
    store(K_FirstRun, mode);
}
//------------------------------------------------------------------------------
bool Settings::showSaveOverlay() {
    return load<bool>(K_ShowSaveOverlay);
}

void Settings::setShowSaveOverlay(bool mode) {
    store(K_ShowSaveOverlay, mode);
}
//------------------------------------------------------------------------------
bool Settings::confirmDelete() {
    return load<bool>(K_ConfirmDelete);
}

void Settings::setConfirmDelete(bool mode) {
    store(K_ConfirmDelete, mode);
}
//------------------------------------------------------------------------------
bool Settings::confirmTrash() {
    return load<bool>(K_ConfirmTrash);
}

void Settings::setConfirmTrash(bool mode) {
    store(K_ConfirmTrash, mode);
}
//------------------------------------------------------------------------------
bool Settings::unloadThumbs() {
    return load<bool>(K_UnloadThumbs);
}

void Settings::setUnloadThumbs(bool mode) {
    store(K_UnloadThumbs, mode);
}
//------------------------------------------------------------------------------
float Settings::zoomStep() {
    bool ok = false;
    float value = settingsConf->value("zoomStep", 0.2f).toFloat(&ok);
    if(!ok)
        return 0.2f;
    value = qBound(0.01f, value, 0.5f);
    return value;
}

void Settings::setZoomStep(float value) {
    value = qBound(0.01f, value, 0.5f);
    store(K_ZoomStep, value);
}
//------------------------------------------------------------------------------
float Settings::mouseScrollingSpeed() {
    bool ok = false;
    float value = settingsConf->value("mouseScrollingSpeed", 1.0f).toFloat(&ok);
    if(!ok)
        return 1.0f;
    value = qBound(0.5f, value, 2.0f);
    return value;
}

void Settings::setMouseScrollingSpeed(float value) {
    value = qBound(0.5f, value, 2.0f);
    store(K_MouseScrollingSpeed, value);
}
//------------------------------------------------------------------------------
void Settings::setZoomIndicatorMode(ZoomIndicatorMode mode) {
    store(K_ZoomIndicatorMode, static_cast<qlonglong>(mode));
}

ZoomIndicatorMode Settings::zoomIndicatorMode() {
    int mode = settingsConf->value("zoomIndicatorMode", 0).toInt();
    if(mode < 0 || mode > 2)
        mode = 0;
    return static_cast<ZoomIndicatorMode>(mode);
}
//------------------------------------------------------------------------------
void Settings::setFocusPointIn1to1Mode(ImageFocusPoint mode) {
    store(K_FocusPointIn1to1Mode, static_cast<qlonglong>(mode));
}

ImageFocusPoint Settings::focusPointIn1to1Mode() {
    int mode = settingsConf->value("focusPointIn1to1Mode", 1).toInt();
    if(mode < 0 || mode > 2)
        mode = 1;
    return static_cast<ImageFocusPoint>(mode);
}

void Settings::setDefaultCropAction(DefaultCropAction mode) {
    store(K_DefaultCropAction, static_cast<qlonglong>(mode));
}

DefaultCropAction Settings::defaultCropAction() {
    int mode = settingsConf->value("defaultCropAction", 0).toInt();
    if(mode < 0 || mode > 1)
        mode = 0;
    return static_cast<DefaultCropAction>(mode);
}

ImageScrolling Settings::imageScrolling() {
    int mode = settingsConf->value("imageScrolling", 1).toInt();
    if(mode < 0 || mode > 2)
        mode = 0;
    return static_cast<ImageScrolling>(mode);
}

void Settings::setImageScrolling(ImageScrolling mode) {
    store(K_ImageScrolling, static_cast<qlonglong>(mode));
}
//------------------------------------------------------------------------------
ViewMode Settings::defaultViewMode() {
    int mode = settingsConf->value("defaultViewMode", 0).toInt();
    if(mode < 0 || mode > 1)
        mode = 0;
    return static_cast<ViewMode>(mode);
}

void Settings::setDefaultViewMode(ViewMode mode) {
    store(K_DefaultViewMode, static_cast<qlonglong>(mode));
}
//------------------------------------------------------------------------------
FolderEndAction Settings::folderEndAction() {
    int mode = settingsConf->value("folderEndAction", 0).toInt();
    if(mode < 0 || mode > 2)
        mode = 0;
    return static_cast<FolderEndAction>(mode);
}

void Settings::setFolderEndAction(FolderEndAction mode) {
    store(K_FolderEndAction, static_cast<qlonglong>(mode));
}
//------------------------------------------------------------------------------
bool Settings::printLandscape() {
    return stateConf->value("printLandscape", false).toBool();
}

void Settings::setPrintLandscape(bool mode) {
    stateConf->setValue("printLandscape", mode);
}
//------------------------------------------------------------------------------
bool Settings::printPdfDefault() {
    return stateConf->value("printPdfDefault", false).toBool();
}

void Settings::setPrintPdfDefault(bool mode) {
    stateConf->setValue("printPdfDefault", mode);
}
//------------------------------------------------------------------------------
bool Settings::printColor() {
    return stateConf->value("printColor", false).toBool();
}

void Settings::setPrintColor(bool mode) {
    stateConf->setValue("printColor", mode);
}
//------------------------------------------------------------------------------
bool Settings::printFitToPage() {
    return stateConf->value("printFitToPage", true).toBool();
}

void Settings::setPrintFitToPage(bool mode) {
    stateConf->setValue("printFitToPage", mode);
}
//------------------------------------------------------------------------------
QString Settings::lastPrinter() {
    return stateConf->value("lastPrinter", "").toString();
}

void Settings::setLastPrinter(const QString& name) {
    stateConf->setValue("lastPrinter", name);
}
//------------------------------------------------------------------------------
bool Settings::jxlAnimation() {
    return load<bool>(K_JxlAnimation);
}

void Settings::setJxlAnimation(bool mode) {
    store(K_JxlAnimation, mode);
}
//------------------------------------------------------------------------------
bool Settings::autoResizeWindow() {
    return load<bool>(K_AutoResizeWindow);
}

void Settings::setAutoResizeWindow(bool mode) {
    store(K_AutoResizeWindow, mode);
}
//------------------------------------------------------------------------------
int Settings::autoResizeLimit() {
    int limit = settingsConf->value("autoResizeLimit", 90).toInt();
    if(limit < 30 || limit > 100)
        limit = 90;
    return limit;
}

void Settings::setAutoResizeLimit(int percent) {
    store(K_AutoResizeLimit, percent);
}
//------------------------------------------------------------------------------
int Settings::memoryAllocationLimit() {
    int limit = settingsConf->value("memoryAllocationLimit", 1024).toInt();
    if(limit < 512)
        limit = 512;
    else if(limit > 8192)
        limit = 8192;
    return limit;
}

void Settings::setMemoryAllocationLimit(int limitMB) {
    store(K_MemoryAllocationLimit, limitMB);
}
//------------------------------------------------------------------------------
bool Settings::panelCenterSelection() {
    return load<bool>(K_PanelCenterSelection);
}

void Settings::setPanelCenterSelection(bool mode) {
    store(K_PanelCenterSelection, mode);
}
//------------------------------------------------------------------------------
QString Settings::language() {
    return load<QString>(K_Language);
}

void Settings::setLanguage(const QString& lang) {
    store(K_Language, lang);
}
//------------------------------------------------------------------------------
bool Settings::useFixedZoomLevels() {
    return load<bool>(K_UseFixedZoomLevels);
}

void Settings::setUseFixedZoomLevels(bool mode) {
    store(K_UseFixedZoomLevels, mode);
}
//------------------------------------------------------------------------------
QString Settings::defaultZoomLevels() {
    return QString("0.05,0.1,0.125,0.166,0.25,0.333,0.5,0.66,1,1.5,2,3,4,5,6,7,8");
}
QString Settings::zoomLevels() {
    return settingsConf->value("fixedZoomLevels", defaultZoomLevels()).toString();
}

void Settings::setZoomLevels(const QString& levels) {
    settingsConf->setValue("fixedZoomLevels", levels);
}
//------------------------------------------------------------------------------
bool Settings::unlockMinZoom() {
    return load<bool>(K_UnlockMinZoom);
}

void Settings::setUnlockMinZoom(bool mode) {
    store(K_UnlockMinZoom, mode);
}
//------------------------------------------------------------------------------
bool Settings::trackpadDetection() {
    return load<bool>(K_TrackpadDetection);
}

void Settings::setTrackpadDetection(bool mode) {
    store(K_TrackpadDetection, mode);
}
//------------------------------------------------------------------------------
bool Settings::clickableEdges() {
    return load<bool>(K_ClickableEdges);
}

void Settings::setClickableEdges(bool mode) {
    store(K_ClickableEdges, mode);
}
//------------------------------------------------------------------------------
bool Settings::clickableEdgesVisible() {
    return load<bool>(K_ClickableEdgesVisible);
}

void Settings::setClickableEdgesVisible(bool mode) {
    store(K_ClickableEdgesVisible, mode);
}
//------------------------------------------------------------------------------
bool Settings::showHiddenFiles() {
    return load<bool>(K_ShowHiddenFiles);
}

void Settings::setShowHiddenFiles(bool mode) {
    store(K_ShowHiddenFiles, mode);
}

// explicit instantiations: the templates are defined in this TU, and the
// table's users (plus the audit test) need the symbols at link time
template bool Settings::load<bool>(const SettingDesc &) const;
template int Settings::load<int>(const SettingDesc &) const;
template qlonglong Settings::load<qlonglong>(const SettingDesc &) const;
template float Settings::load<float>(const SettingDesc &) const;
template double Settings::load<double>(const SettingDesc &) const;
template QString Settings::load<QString>(const SettingDesc &) const;
template void Settings::store<bool>(const SettingDesc &, const bool &);
template void Settings::store<int>(const SettingDesc &, const int &);
template void Settings::store<qlonglong>(const SettingDesc &, const qlonglong &);
template void Settings::store<float>(const SettingDesc &, const float &);
template void Settings::store<double>(const SettingDesc &, const double &);
template void Settings::store<QString>(const SettingDesc &, const QString &);
