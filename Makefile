# st - simple terminal
# See LICENSE file for copyright and license details.

include config.mk

SRC = st.c std.c util.c pty.c
OBJ = ${SRC:.c=.o}

all: options st std

options:
	@echo st build options:
	@echo "CFLAGS     = ${CFLAGS}"
	@echo "LDFLAGS    = ${LDFLAGS}"
	@echo "X11LDFLAGS = ${X11LDFLAGS}"
	@echo "CC         = ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

${OBJ}: config.mk

st: st.o util.o
	@echo CC -o $@
	@${CC} -o $@ $^ ${LDFLAGS} ${X11LDFLAGS}

std: std.o pty.o util.o
	@echo CC -o $@
	@${CC} -o $@ $^ ${LDFLAGS}

clean:
	@echo cleaning
	@rm -f st std ${OBJ} st-${VERSION}.tar.gz

dist: clean
	@echo creating dist tarball
	@mkdir -p st-${VERSION}
	@cp -R LICENSE Makefile README config.mk \
		st.1 ${SRC} st-${VERSION}
	@tar -cf st-${VERSION}.tar st-${VERSION}
	@gzip st-${VERSION}.tar
	@rm -rf st-${VERSION}

install: all
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@cp -f st ${DESTDIR}${PREFIX}/bin
	@cp -f std ${DESTDIR}${PREFIX}/bin
	@chmod 755 ${DESTDIR}${PREFIX}/bin/st
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@sed "s/VERSION/${VERSION}/g" < st.1 > ${DESTDIR}${MANPREFIX}/man1/st.1
	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/st.1
	@sed "s/VERSION/${VERSION}/g" < std.1 > ${DESTDIR}${MANPREFIX}/man1/std.1
	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/std.1

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/st
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/st.1

.PHONY: all options clean dist install uninstall
