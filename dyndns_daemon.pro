TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt
CONFIG += c++11
CONFIG += precompiled_header

CONFIG(release,debug|release) { DEFINES += NDEBUG RELEASE } # ATTN: curly brace must start in same line!
CONFIG(debug,debug|release) { DEFINES += DEBUG } # ATTN: curly brace must start in same line!

QMAKE_CXXFLAGS_RELEASE += -Os

LIBS += -lpthread
LIBS += -lcurl


INCLUDEPATH +=	\
	Source	\
	Libraries


SOURCES += \
	Libraries/unix/FD.cpp \
	Source/main.cpp \
	Libraries/kio/kio.cpp \
	Libraries/kio/exceptions.cpp \
	Libraries/cstrings/cstrings.cpp \
	Libraries/cstrings/tempmem.cpp \
	Libraries/unix/files.cpp \
	Libraries/unix/log.cpp \


HEADERS += \
	Libraries/unix/FD.h \
	Source/settings.h \
	Source/main.h \
	Libraries/kio/exceptions.h \
	Libraries/kio/standard_types.h \
	Libraries/kio/nice_defines.h \
	Libraries/kio/detect_configuration.h \
	Libraries/kio/auto_config.h \
	Libraries/kio/kio.h \
	Libraries/kio/errors.h \
	Libraries/kio/error_emacs.h \
	Libraries/cstrings/cstrings.h \
	Libraries/cstrings/tempmem.h \
	Libraries/unix/files.h \
	Libraries/unix/log.h \
	Libraries/Templates/Array.h \



