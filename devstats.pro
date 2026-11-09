QT += core gui widgets network sql dbus
CONFIG += c++17 warn_on
TEMPLATE = app
TARGET = devstats
VERSION = $$cat(VERSION, lines)
# Make the version a real compiler dependency, not a cached command-line flag.
VERSION_HEADER = $$OUT_PWD/devstats_version.h
VERSION_HEADER_CONTENT = $$cat($$PWD/src/devstats_version.h.in, lines)
VERSION_HEADER_CONTENT = $$replace(VERSION_HEADER_CONTENT, @DEVSTATS_VERSION@, $$VERSION)
exists($$VERSION_HEADER): VERSION_HEADER_PREVIOUS = $$cat($$VERSION_HEADER, lines)
!equals(VERSION_HEADER_PREVIOUS, $$VERSION_HEADER_CONTENT) {
    !write_file($$VERSION_HEADER, VERSION_HEADER_CONTENT): error(Cannot write version header)
}
INCLUDEPATH += $$OUT_PWD
HEADERS += $$VERSION_HEADER
QMAKE_INTERNAL_INCLUDED_FILES += $$PWD/VERSION $$PWD/src/devstats_version.h.in
SOURCES += $$files(src/*.cpp)
HEADERS += $$files(src/*.h)
RESOURCES += src/assets/assets.qrc
unix:!macx {
    QMAKE_LFLAGS += -Wl,--as-needed
    target.path = /usr/bin
    INSTALLS += target
}
