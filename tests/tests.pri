# Every test links the whole application except its entry point. For faster runs,
# scripts/run-tests.py compiles each test once against existing production objects.
QT += core gui widgets network sql dbus testlib
CONFIG += c++17 console warn_on
CONFIG -= app_bundle
TEMPLATE = app
INCLUDEPATH += ../src
SOURCES += $$files(../src/*.cpp)
SOURCES -= ../src/main.cpp
HEADERS += $$files(../src/*.h)
RESOURCES += ../src/assets/assets.qrc
