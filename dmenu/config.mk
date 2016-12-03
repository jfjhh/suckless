# dmenu version
VERSION = 4.5-tip

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

# Pango
XFTINC = `pkg-config --cflags xft pango pangoxft`
XFTLIBS  = -lXrender -lfreetype -lz `pkg-config --libs xft pango pangoxft`

# Imlib
IMLIBINC = `pkg-config --cflags imlib2 openssl`
IMLIBLIBS = `pkg-config --libs imlib2 openssl`

# Xinerama, comment if you don't want it
XINERAMALIBS  = -lXinerama
XINERAMAFLAGS = -DXINERAMA

# includes and libs
INCS = -I${X11INC} ${XFTINC} ${IMLIBINC}
LIBS = -L${X11LIB} -lX11 ${XINERAMALIBS} ${XFTLIBS} ${IMLIBLIBS}

# flags
CPPFLAGS = -DFORTIFY_SOURCE -D_BSD_SOURCE -D_POSIX_C_SOURCE=200809L -DVERSION=\"${VERSION}\" ${XINERAMAFLAGS}
CFLAGS   = -std=c99 -pedantic -Wall -Os ${INCS} ${CPPFLAGS}
LDFLAGS  = -s ${LIBS}

# compiler and linker
CC = cc
