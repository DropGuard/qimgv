#pragma once

#include <QDebug>
#include <QColor>
#include <QPalette>

enum ColorSchemes {
    COLORS_SYSTEM,
    COLORS_LIGHT,
    COLORS_BLACK,
    COLORS_DARK,
    COLORS_DARKBLUE,
    COLORS_CUSTOMIZED
};


struct BaseColorScheme {
    int tid;
    QColor background;
    QColor background_fullscreen;
    QColor text;
    QColor icons;
    QColor widget;
    QColor widget_border;
    QColor accent;
    QColor folderview;
    QColor folderview_topbar;
    QColor scrollbar;
    QColor overlay_text;
    QColor overlay;
};

class ColorScheme {
public:
    ColorScheme();
    ColorScheme(BaseColorScheme base);
    void setBaseColors(BaseColorScheme base);
    // index of theme name
    int tid;
    // base
    QColor background;
    QColor background_fullscreen;
    QColor text;
    QColor icons;
    QColor widget;
    QColor widget_border;
    QColor accent;
    QColor folderview;
    QColor folderview_topbar;
    QColor scrollbar;
    QColor scrollbar_hover;
    QColor overlay_text;
    QColor overlay;
    // extended
    QColor text_hc2;
    QColor text_hc;
    QColor text_lc;
    QColor text_lc2;
    QColor button;
    QColor button_hover;
    QColor button_pressed;
    QColor panel_button;
    QColor panel_button_hover;
    QColor panel_button_pressed;
    QColor folderview_hc;
    QColor folderview_hc2;
    QColor folderview_button_hover;
    QColor folderview_button_pressed;
    QColor input_field_focus;


private:
    void createColorVariants();
};

// The declarative table of theme colors: one row per persisted color role.
// loadTheme/saveTheme loop over it; a junk color from a hand-edited theme
// file falls back to the row default (same contract as settings_table.h).
// Both member pointers ride together because ColorScheme re-declares the
// base roles instead of inheriting them.
struct ThemeColorDesc {
    const char *key;
    const char *def;
    QColor BaseColorScheme::*base;
    QColor ColorScheme::*scheme;
};

inline constexpr ThemeColorDesc kThemeColors[] = {
    {"background",            "#1a1a1a", &BaseColorScheme::background,            &ColorScheme::background},
    {"background_fullscreen", "#1a1a1a", &BaseColorScheme::background_fullscreen, &ColorScheme::background_fullscreen},
    {"text",                  "#b6b6b6", &BaseColorScheme::text,                  &ColorScheme::text},
    {"icons",                 "#a4a4a4", &BaseColorScheme::icons,                 &ColorScheme::icons},
    {"widget",                "#252525", &BaseColorScheme::widget,                &ColorScheme::widget},
    {"widget_border",         "#2c2c2c", &BaseColorScheme::widget_border,         &ColorScheme::widget_border},
    {"accent",                "#8c9b81", &BaseColorScheme::accent,                &ColorScheme::accent},
    {"folderview",            "#242424", &BaseColorScheme::folderview,            &ColorScheme::folderview},
    {"folderview_topbar",     "#383838", &BaseColorScheme::folderview_topbar,     &ColorScheme::folderview_topbar},
    {"scrollbar",             "#5a5a5a", &BaseColorScheme::scrollbar,             &ColorScheme::scrollbar},
    {"overlay_text",          "#d2d2d2", &BaseColorScheme::overlay_text,          &ColorScheme::overlay_text},
    {"overlay",               "#1a1a1a", &BaseColorScheme::overlay,               &ColorScheme::overlay},
};

class ThemeStore {
public:
    static ColorScheme colorScheme(ColorSchemes name);
};
