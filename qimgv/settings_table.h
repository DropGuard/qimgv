#pragma once

#include <QtGlobal>

// The declarative table of scalar leaf settings. One row per key: the key, its
// type, its default, and (for numbers) the valid range. The typed accessors in
// Settings (load/store) read this table, so a hand-edited config value can
// never flow past a clamp again, and the audit test walks every row.
//
// NOT in the table, deliberately: composite or computed values (scripts,
// videoFormats, supportedFormats, cacheDir fallback, scalingFilter's
// build-conditional default, zoomLevels) — hard-casing them into a scalar
// table would recreate the boilerplate the table exists to remove.

struct SettingDesc {
    enum class Kind : unsigned char { Bool, Int, Real, Str };
    const char *key = nullptr;
    Kind kind = Kind::Bool;
    // four parallel defaults instead of a union: switching a union's active
    // member is not a constant expression before C++20
    bool defB = false;
    qlonglong defI = 0;
    double defR = 0.0;
    const char *defS = nullptr;
    double lo = 0.0, hi = 0.0;      // numeric clamp bounds
    bool hasLo = false, hasHi = false;
    // Two policies exist for a value outside [lo, hi] and they answer
    // differently: clamp moves it to the nearest legal value, reset discards it
    // for the default. Enum-valued settings want reset (an out-of-range mode is
    // a broken value, not a request for the last mode), a pixel size wants
    // clamp (60 is a real size that only overshot the panel minimum).
    bool resetOnRange = false;

    constexpr bool clamped() const { return hasLo || hasHi; }
};

namespace SettingRow {
static constexpr SettingDesc BOOL(const char *key, bool def) {
    SettingDesc d;
    d.key = key; d.kind = SettingDesc::Kind::Bool; d.defB = def;
    return d;
}
static constexpr SettingDesc INT(const char *key, qlonglong def) {
    SettingDesc d;
    d.key = key; d.kind = SettingDesc::Kind::Int; d.defI = def;
    return d;
}
static constexpr SettingDesc INT(const char *key, qlonglong def, qlonglong lo) {
    SettingDesc d;
    d.key = key; d.kind = SettingDesc::Kind::Int; d.defI = def;
    d.lo = lo; d.hasLo = true;
    return d;
}
static constexpr SettingDesc INT(const char *key, qlonglong def, qlonglong lo, qlonglong hi) {
    SettingDesc d;
    d.key = key; d.kind = SettingDesc::Kind::Int; d.defI = def;
    d.lo = lo; d.hi = hi; d.hasLo = true; d.hasHi = true;
    return d;
}
// the same bounds, the other policy: out of range means "take the default"
static constexpr SettingDesc INT_RESET(const char *key, qlonglong def, qlonglong lo) {
    SettingDesc d;
    d.key = key; d.kind = SettingDesc::Kind::Int; d.defI = def;
    d.lo = lo; d.hasLo = true; d.resetOnRange = true;
    return d;
}
static constexpr SettingDesc INT_RESET(const char *key, qlonglong def, qlonglong lo, qlonglong hi) {
    SettingDesc d;
    d.key = key; d.kind = SettingDesc::Kind::Int; d.defI = def;
    d.lo = lo; d.hi = hi; d.hasLo = true; d.hasHi = true; d.resetOnRange = true;
    return d;
}
static constexpr SettingDesc REAL(const char *key, double def, double lo, double hi) {
    SettingDesc d;
    d.key = key; d.kind = SettingDesc::Kind::Real; d.defR = def;
    d.lo = lo; d.hi = hi; d.hasLo = true; d.hasHi = true;
    return d;
}
static constexpr SettingDesc STR(const char *key, const char *def) {
    SettingDesc d;
    d.key = key; d.kind = SettingDesc::Kind::Str; d.defS = def;
    return d;
}
} // namespace SettingRow

using SettingRow::BOOL;
using SettingRow::INT;
using SettingRow::INT_RESET;
using SettingRow::REAL;
using SettingRow::STR;

// ---- rows -------------------------------------------------------------------

inline constexpr SettingDesc K_VideoPlayback          = BOOL("videoPlayback", true);
inline constexpr SettingDesc K_UseSystemColorScheme   = BOOL("useSystemColorScheme", false);
inline constexpr SettingDesc K_ShowChangelogs         = BOOL("showChangelogs", true);
inline constexpr SettingDesc K_BlurBackground         = BOOL("blurBackground", true);
inline constexpr SettingDesc K_PlayVideoSounds        = BOOL("playVideoSounds", false);
inline constexpr SettingDesc K_UsePreloader           = BOOL("usePreloader", true);
inline constexpr SettingDesc K_KeepFitMode            = BOOL("keepFitMode", false);
inline constexpr SettingDesc K_FullscreenMode         = BOOL("openInFullscreen", false);
inline constexpr SettingDesc K_PanelEnabled           = BOOL("panelEnabled", true);
inline constexpr SettingDesc K_PanelFullscreenOnly    = BOOL("panelFullscreenOnly", true);
inline constexpr SettingDesc K_PanelPinned            = BOOL("panelPinned", false);
inline constexpr SettingDesc K_LoopSlideshow          = BOOL("loopSlideshow", false);
inline constexpr SettingDesc K_SquareThumbnails       = BOOL("squareThumbnails", false);
inline constexpr SettingDesc K_TransparencyGrid       = BOOL("drawTransparencyGrid", false);
inline constexpr SettingDesc K_EnableSmoothScroll     = BOOL("enableSmoothScroll", true);
inline constexpr SettingDesc K_UseThumbnailCache      = BOOL("thumbnailCache", true);
inline constexpr SettingDesc K_SmoothUpscaling        = BOOL("smoothUpscaling", true);
inline constexpr SettingDesc K_ExpandImage            = BOOL("expandImage", false);
inline constexpr SettingDesc K_SmoothAnimatedImages   = BOOL("smoothAnimatedImages", true);
inline constexpr SettingDesc K_InfoBarFullscreen      = BOOL("infoBarFullscreen", true);
inline constexpr SettingDesc K_InfoBarWindowed        = BOOL("infoBarWindowed", false);
inline constexpr SettingDesc K_WindowTitleExtendedInfo = BOOL("windowTitleExtendedInfo", true);
inline constexpr SettingDesc K_CursorAutohide         = BOOL("cursorAutohiding", true);
inline constexpr SettingDesc K_FirstRun               = BOOL("firstRun", true);
inline constexpr SettingDesc K_ShowSaveOverlay        = BOOL("showSaveOverlay", true);
inline constexpr SettingDesc K_ConfirmDelete          = BOOL("confirmDelete", true);
inline constexpr SettingDesc K_ConfirmTrash           = BOOL("confirmTrash", true);
inline constexpr SettingDesc K_UnloadThumbs           = BOOL("unloadThumbs", true);
inline constexpr SettingDesc K_JxlAnimation           = BOOL("jxlAnimation", false);
inline constexpr SettingDesc K_AutoResizeWindow       = BOOL("autoResizeWindow", false);
inline constexpr SettingDesc K_UseFixedZoomLevels     = BOOL("useFixedZoomLevels", false);
inline constexpr SettingDesc K_UnlockMinZoom          = BOOL("unlockMinZoom", true);
inline constexpr SettingDesc K_TrackpadDetection      = BOOL("trackpadDetection", true);
inline constexpr SettingDesc K_ClickableEdges         = BOOL("clickableEdges", false);
inline constexpr SettingDesc K_ClickableEdgesVisible  = BOOL("clickableEdgesVisible", true);
inline constexpr SettingDesc K_ShowHiddenFiles        = BOOL("showHiddenFiles", false);
inline constexpr SettingDesc K_PanelCenterSelection   = BOOL("panelCenterSelection", false);
inline constexpr SettingDesc K_FolderViewIconSize     = INT("folderViewIconSize", 120);
inline constexpr SettingDesc K_DefaultFitMode         = INT_RESET("defaultFitMode", 0, 0, 3);
inline constexpr SettingDesc K_FocusPointIn1to1Mode   = INT_RESET("focusPointIn1to1Mode", 1, 0, 2);
inline constexpr SettingDesc K_DefaultCropAction      = INT_RESET("defaultCropAction", 0, 0, 1);
inline constexpr SettingDesc K_DefaultViewMode        = INT_RESET("defaultViewMode", 0, 0, 1);
inline constexpr SettingDesc K_SortingMode            = INT_RESET("sortingMode", 0, 0, 5);
inline constexpr SettingDesc K_FolderEndAction        = INT_RESET("folderEndAction", 0, 0, 2);
inline constexpr SettingDesc K_FolderViewMode         = INT_RESET("folderViewMode", 2, 0, 2);
inline constexpr SettingDesc K_ThumbPanelStyle        = INT_RESET("thumbPanelStyle", 1, 0, 1);
inline constexpr SettingDesc K_ImageScrolling         = INT_RESET("imageScrolling", 1, 0, 2);
inline constexpr SettingDesc K_ZoomIndicatorMode      = INT_RESET("zoomIndicatorMode", 0, 0, 2);
inline constexpr SettingDesc K_MemoryAllocationLimit  = INT("memoryAllocationLimit", 1024, 512, 8192);
inline constexpr SettingDesc K_SlideshowInterval      = INT_RESET("slideshowInterval", 3000, 1);
inline constexpr SettingDesc K_ThumbnailerThreads     = INT_RESET("thumbnailerThreads", 4, 1);
inline constexpr SettingDesc K_PanelPreviewsSize      = INT("panelPreviewsSize", 140, 100, 250);
inline constexpr SettingDesc K_AutoResizeLimit        = INT_RESET("autoResizeLimit", 90, 30, 100);
inline constexpr SettingDesc K_JPEGSaveQuality        = INT("JPEGSaveQuality", 95, 0, 100);
inline constexpr SettingDesc K_ExpandLimit            = INT("expandLimit", 2);
inline constexpr SettingDesc K_LastVerMajor           = INT("lastVerMajor", 0);
inline constexpr SettingDesc K_LastVerMinor           = INT("lastVerMinor", 0);
inline constexpr SettingDesc K_LastVerMicro           = INT("lastVerMicro", 0);

inline constexpr SettingDesc K_ZoomStep               = REAL("zoomStep", 0.2, 0.01, 0.5);
inline constexpr SettingDesc K_MouseScrollingSpeed    = REAL("mouseScrollingSpeed", 1.0, 0.5, 2.0);
inline constexpr SettingDesc K_BackgroundOpacity      = REAL("backgroundOpacity", 1.0, 0.0, 1.0);

inline constexpr SettingDesc K_PanelPosition          = STR("panelPosition", "top");
inline constexpr SettingDesc K_MpvBinary              = STR("mpvBinary", "");
inline constexpr SettingDesc K_Language               = STR("language", "en_US");

// The audit test walks this list; keep it in sync when adding a row.
inline constexpr const SettingDesc *kAllSettings[] = {
    &K_VideoPlayback, &K_UseSystemColorScheme, &K_ShowChangelogs, &K_BlurBackground,
    &K_PlayVideoSounds, &K_UsePreloader, &K_KeepFitMode, &K_FullscreenMode,
    &K_PanelEnabled, &K_PanelFullscreenOnly, &K_PanelPinned, &K_LoopSlideshow,
    &K_SquareThumbnails, &K_TransparencyGrid, &K_EnableSmoothScroll, &K_UseThumbnailCache,
    &K_SmoothUpscaling, &K_ExpandImage, &K_SmoothAnimatedImages, &K_InfoBarFullscreen,
    &K_InfoBarWindowed, &K_WindowTitleExtendedInfo, &K_CursorAutohide, &K_FirstRun,
    &K_ShowSaveOverlay, &K_ConfirmDelete, &K_ConfirmTrash, &K_UnloadThumbs,
    &K_JxlAnimation, &K_AutoResizeWindow, &K_UseFixedZoomLevels, &K_UnlockMinZoom,
    &K_TrackpadDetection, &K_ClickableEdges, &K_ClickableEdgesVisible, &K_ShowHiddenFiles,
    &K_SortingMode, &K_FolderEndAction, &K_FolderViewMode, &K_ThumbPanelStyle,
    &K_ImageScrolling, &K_ZoomIndicatorMode, &K_MemoryAllocationLimit, &K_SlideshowInterval,
    &K_ThumbnailerThreads, &K_PanelPreviewsSize, &K_AutoResizeLimit, &K_JPEGSaveQuality,
    &K_ExpandLimit, &K_LastVerMajor, &K_LastVerMinor, &K_LastVerMicro,
    &K_ZoomStep, &K_MouseScrollingSpeed, &K_BackgroundOpacity,
    &K_PanelPosition, &K_MpvBinary, &K_Language,
};
