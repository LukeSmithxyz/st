# st version
VERSION = 0.2.1-tip

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

# includes and libs
INCS = -I. -I/usr/include -I${X11INC} -I/usr/include/freetype2
LIBS = -L/usr/lib -lc -L${X11LIB} -lX11 -lutil -lXext -lXft -lfontconfig

# flags
CPPFLAGS = -DVERSION=\"${VERSION}\"
CFLAGS += -g -std=c99 -pedantic -Wall -Os ${INCS} ${CPPFLAGS}
LDFLAGS += -g ${LIBS}

# compiler and linker
CC ?= cc
