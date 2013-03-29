# st version
VERSION = 0.4

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

# includes and libs
INCS = -I. -I/usr/include -I${X11INC} \
       $(shell pkg-config --cflags fontconfig) \
       $(shell pkg-config --cflags freetype2)
LIBS = -L/usr/lib -lc -L${X11LIB} -lX11 -lutil -lXext -lXft \
       $(shell pkg-config --libs fontconfig)  \
       $(shell pkg-config --libs freetype2)

# flags
CPPFLAGS = -DVERSION=\"${VERSION}\" -D_BSD_SOURCE -D_XOPEN_SOURCE=600
CFLAGS += -g -std=c99 -pedantic -Wall -Wvariadic-macros -Os ${INCS} ${CPPFLAGS}
LDFLAGS += -g ${LIBS}

# compiler and linker
CC ?= cc

