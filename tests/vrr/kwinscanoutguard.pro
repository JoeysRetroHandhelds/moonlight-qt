TEMPLATE = app
TARGET = tst_kwinscanoutguard
QT -= gui
CONFIG += console c++17 link_pkgconfig
# libwayland-client is deliberately not linked: the guard loads it at runtime,
# as in the AppImage.
PKGCONFIG += wayland-server
QMAKE_CXXFLAGS += $$system(pkg-config --cflags wayland-client)
INCLUDEPATH += $$PWD/../../app $$PWD
SOURCES += $$PWD/tst_kwinscanoutguard.cpp \
    $$PWD/../../app/streaming/kwinscanoutguard.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/protocols/kde-output-device-v2-protocol.c \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/protocols/kde-output-management-v2-protocol.c
HEADERS += $$PWD/protocols/kde-output-device-v2-server-protocol.h \
    $$PWD/protocols/kde-output-management-v2-server-protocol.h
LIBS += -ldl -pthread
# Assertions exercise protocol/lifecycle invariants even in release builds.
QMAKE_CXXFLAGS_RELEASE -= -DNDEBUG
