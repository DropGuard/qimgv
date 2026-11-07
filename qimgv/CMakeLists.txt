include(GNUInstallDirs)

set(CMAKE_VERBOSE_MAKEFILE ON)

if(APPLE)
    set(APPLE_ICON_NAME "qimgv.icns")
    set(APPLE_ICON_PATH "distrib/${APPLE_ICON_NAME}")
endif()

# TRANSLATIONS
find_package(Qt6 COMPONENTS LinguistTools)
# Example of creating a new translation file.
# 1. cd into qimgv directory and run:
#   lupdate -recursive . -ts res/translations/qimgv_en_US.ts    [replace "en_US" with your locale]
# 2. Add your .ts filename into TS_FILES list below
# 3. Translate it with Qt Linguist
set(TS_FILES
    "zh_CN.ts"
    "uk_UA.ts"
    "es_ES.ts"
    "de_DE.ts"
    "fr_FR.ts"
    "tr_TR.ts"
    "ja_JP.ts"
)
list(TRANSFORM TS_FILES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/res/translations/)
set_source_files_properties(${TS_FILES} PROPERTIES OUTPUT_LOCATION ${PROJECT_BINARY_DIR}/qimgv/translations)

# ADD EXECUTABLE
# Everything that does not need the widget tree lives in the qimgv_nongui object
# library below, so test binaries can link the real components instead of a
# hand-copied list of source files.
add_executable(qimgv
    main.cpp
    macosapplication.cpp
    qimgv.rc
    ${APPLE_ICON_PATH}
    ${QM_FILES}
)

# 1. 'qimgv_lupdate' updates the .ts files (run it manually)
# 2. .ts are compiled to .qm by qt_add_lrelease
qt_add_lupdate(qimgv TS_FILES ${TS_FILES})
qt_add_lrelease(qimgv TS_FILES ${TS_FILES})

# ADD SOURCES
# An OBJECT library rather than a static one: the sources below stay compiled
# once and are consumed directly by both the executable and the test binaries.
add_library(qimgv_nongui OBJECT)

target_sources(qimgv_nongui PRIVATE
    appversion.cpp
    settings.cpp
    sharedresources.cpp
    shortcutbuilder.cpp
    themestore.cpp
)

target_include_directories(qimgv_nongui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/qimgv-engine-ffi/include)
target_link_libraries(qimgv_nongui PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    Qt6::PrintSupport
    qimgv_engine_ffi
    qimgv_sanitizers)

# OPTION DEFINITIONS, LINKING (mirrored from the executable below: these are the
# same translation units that used to be compiled as part of the qimgv target)
if(EXIV2)
    target_link_libraries(qimgv_nongui PUBLIC PkgConfig::Exiv2)
    target_compile_definitions(qimgv_nongui PRIVATE USE_EXIV2)
endif()
if(KDE_SUPPORT)
    target_link_libraries(qimgv_nongui PUBLIC KF6::WindowSystem)
    target_compile_definitions(qimgv_nongui PRIVATE USE_KDE_BLUR)
endif()
if(VIDEO_SUPPORT)
    target_compile_definitions(qimgv_nongui PRIVATE USE_MPV)
endif()
if(OPENCV_SUPPORT)
    target_link_libraries(qimgv_nongui PUBLIC ${OpenCV_LIBS})
    target_compile_definitions(qimgv_nongui PRIVATE USE_OPENCV)
endif()

# The widget tree plus Core: its own object library so that a test binary can
# construct a real Core and drive its navigation without a display server.
add_library(qimgv_gui OBJECT)
target_sources(qimgv_gui PRIVATE
    core.cpp
    proxystyle.cpp
    resources.qrc
)
target_link_libraries(qimgv_gui PUBLIC qimgv_nongui qimgv_sanitizers Qt6::OpenGLWidgets)
if(EXIV2)
    target_compile_definitions(qimgv_gui PRIVATE USE_EXIV2)
endif()
if(KDE_SUPPORT)
    target_compile_definitions(qimgv_gui PRIVATE USE_KDE_BLUR)
endif()
if(VIDEO_SUPPORT)
    target_compile_definitions(qimgv_gui PRIVATE USE_MPV)
endif()
if(OPENCV_SUPPORT)
    target_compile_definitions(qimgv_gui PRIVATE USE_OPENCV)
endif()

add_subdirectory(components)
add_subdirectory(sourcecontainers)
add_subdirectory(utils)
add_subdirectory(gui)
add_subdirectory(tests)
if(OPENCV_SUPPORT)
    add_subdirectory(3rdparty/QtOpenCV)
endif()

# C++ STANDARD

# LINK STUFF
target_link_libraries(qimgv PRIVATE qimgv_gui qimgv_nongui qimgv_sanitizers)
target_link_libraries(qimgv PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg Qt6::PrintSupport)
# Rust engine (corrosion-imported; target name normalized to underscores)
target_link_libraries(qimgv PRIVATE qimgv_engine_ffi)
target_include_directories(qimgv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../engine/qimgv-engine-ffi/include)
# OPTION DEFINITIONS, LINKING
if(EXIV2)
    target_link_libraries(qimgv PRIVATE PkgConfig::Exiv2)
    target_compile_definitions(qimgv PRIVATE USE_EXIV2)
endif()
if(KDE_SUPPORT)
    target_link_libraries(qimgv PRIVATE KF6::WindowSystem)
    target_compile_definitions(qimgv PRIVATE USE_KDE_BLUR)
endif()
if(VIDEO_SUPPORT)
    target_compile_definitions(qimgv PRIVATE USE_MPV)
    if(WIN32)
        ADD_DEFINITIONS(-D_QIMGV_PLUGIN_DIR="plugins")
        ADD_DEFINITIONS(-D_QIMGV_PLAYER_PLUGIN="player_mpv.dll")
    else()
        ADD_DEFINITIONS(-D_QIMGV_PLUGIN_DIR="${QIMGV_PLUGIN_DIR}")
        ADD_DEFINITIONS(-D_QIMGV_PLAYER_PLUGIN="player_mpv.so")
    endif()
endif()
if(OPENCV_SUPPORT)
    target_link_libraries(qimgv PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(qimgv PRIVATE USE_OPENCV)
endif()

# generate proper GUI program on specified platform
if(WIN32) # Check if we are on Windows
    if(MSVC) # Check if we are using the Visual Studio compiler
        set_target_properties(qimgv PROPERTIES WIN32_EXECUTABLE ON)
        target_link_options(qimgv PRIVATE "/ENTRY:mainCRTStartup")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(qimgv PRIVATE "-mwindows")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
        set(CMAKE_CXX_FLAGS "$ENV{CMAKE_CXX_FLAGS} -Wno-shift-negative-value")
        set(CMAKE_EXE_LINKER_FLAGS "$ENV{CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
        target_link_options(qimgv PRIVATE "-mwindows")
    else()
        message(SEND_ERROR "You are using an unsupported Windows compiler! (Not MSVC or GCC)")
    endif(MSVC)
elseif(APPLE)
    set_target_properties(qimgv PROPERTIES
        MACOSX_BUNDLE YES
        MACOSX_BUNDLE_BUNDLE_NAME "qimgv"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.2"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.2"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/distrib/MyBundleInfo.plist.in
        MACOSX_BUNDLE_ICON_FILE ${APPLE_ICON_NAME}
    )
    set_source_files_properties(${APPLE_ICON_PATH} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
elseif(NOT UNIX)
    message(SEND_ERROR "You are on an unsupported platform! (Not Win32, Mac OS X or Unix)")
endif()

## INSTALLATION
set_target_properties(qimgv PROPERTIES OUTPUT_NAME impulse)
install(TARGETS qimgv DESTINATION bin)

# .desktop
install(FILES distrib/impulse.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# icons
foreach(icon IN ITEMS 16x16 22x22 24x24 32x32 36x36 48x48 64x64 128x128 256x256)
    install(FILES distrib/hicolor/${icon}/apps/impulse.png DESTINATION
        ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${icon}/apps)
endforeach()
install(FILES distrib/hicolor/scalable/apps/impulse.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)

# AppData manifest
install(FILES distrib/impulse.appdata.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)

# Install internationalization language files
if(WIN32)
    install(DIRECTORY "${PROJECT_BINARY_DIR}/qimgv/translations" DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
    ADD_DEFINITIONS(-DTRANSLATIONS_PATH="${CMAKE_INSTALL_FULL_DATAROOTDIR}/impulse/translations")
    install(DIRECTORY "${PROJECT_BINARY_DIR}/qimgv/translations" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/impulse")
endif()

# uninstall target
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()
