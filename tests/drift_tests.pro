QT += core gui network quick quickcontrols2 testlib
CONFIG += c++17 testcase
CONFIG -= app_bundle # macOS: bin/test runs ./drift_tests
TARGET = drift_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/discord.h ../src/mpv.h ../src/process.h ../src/theme.h ../src/translator.h
SOURCES += drift_tests.cpp ../src/backend.cpp ../src/discord.cpp ../src/mpv.cpp ../src/theme.cpp ../src/translator.cpp

# MPRIS: media keys and Omarchy's bar. Windows gets its own media controls separately.
linux {
    QT += dbus
    HEADERS += ../src/mpris.h
    SOURCES += ../src/mpris.cpp
}
