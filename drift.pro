QT += core gui network qml quick quickcontrols2

CONFIG += c++17 release
TARGET = drift
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/mpv.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/mpv.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
