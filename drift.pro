QT += core gui network qml quick quickcontrols2

CONFIG += c++17 release
TARGET = drift
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/mpv.h \
    src/process.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/mpv.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc

win32 {
    CONFIG += windows
    RC_ICONS = pkgbuild/drift.ico
}

# MPRIS: media keys and Omarchy's bar. Windows gets its own media controls separately.
linux {
    QT += dbus
    HEADERS += src/mpris.h
    SOURCES += src/mpris.cpp
}
