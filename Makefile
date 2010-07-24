# st - simple terminal
# See LICENSE file for copyright and license details.

include config.mk

SRC = st.c
OBJ = ${SRC:.c=.o}

all: options st

options:
	@echo st build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk

st: ${OBJ}
	@echo CC -o $@
	@${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	@echo cleaning
	@rm -f st ${OBJ} st-${VERSION}.tar.gz

dist: clean
	@echo creating dist tarball
	@mkdir -p st-${VERSION}
	@cp -R LICENSE Makefile README config.mk st.h ${SRC} st-${VERSION}
	@tar -cf st-${VERSION}.tar st-${VERSION}
	@gzip st-${VERSION}.tar
	@rm -rf st-${VERSION}

install: all
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@cp -f st ${DESTDIR}${PREFIX}/bin
	@chmod 755 ${DESTDIR}${PREFIX}/bin/st
	@tic st.info
	@tic st-256color.info

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/st

.PHONY: all options clean dist install uninstall
