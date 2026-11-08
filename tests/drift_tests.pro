QT += core gui network quick quickcontrols2 testlib
CONFIG += c++17 testcase
TARGET = drift_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/mpv.h ../src/process.h ../src/theme.h ../src/translator.h
SOURCES += drift_tests.cpp ../src/backend.cpp ../src/mpv.cpp ../src/theme.cpp ../src/translator.cpp
