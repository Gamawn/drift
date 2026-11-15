QT += core gui network qml quick quickcontrols2

CONFIG += c++17 release
TARGET = drift
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/discord.h \
    src/mpv.h \
    src/process.h \
    src/theme.h \
    src/translator.h \
    src/updater.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/discord.cpp \
    src/mpv.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/updater.cpp

RESOURCES += src/resources.qrc

win32 {
    CONFIG += windows
    RC_ICONS = pkgbuild/drift.ico
    # Media keys and the media flyout; C++/WinRT ships with the Windows SDK and
    # needs C++20, as MSVC refuses the experimental coroutines it uses under C++17.
    CONFIG -= c++17
    CONFIG += c++20
    QT += widgets # the tray icon
    HEADERS += src/smtc.h
    SOURCES += src/smtc.cpp
    LIBS += -lwindowsapp -luser32 # user32: the global hotkeys
}

# MPRIS: media keys and Omarchy's bar. Windows gets its own media controls separately.
linux {
    QT += dbus
    HEADERS += src/mpris.h
    SOURCES += src/mpris.cpp
}

# macOS: build/Drift.app, with the media keys and Control Center's Now Playing.
macx {
    TARGET = Drift
    ICON = pkgbuild/drift.icns
    QMAKE_INFO_PLIST = macos/Info.plist
    VERSION = $$system(sed -n 's/^pkgver=//p' $$PWD/pkgbuild/PKGBUILD)
    HEADERS += src/nowplaying.h
    SOURCES += src/nowplaying.mm
    QMAKE_CXXFLAGS += -fobjc-arc # nowplaying.mm; plain C++ ignores it
    LIBS += -framework MediaPlayer -framework AppKit
}

DISTFILES += \
    TODO.md
