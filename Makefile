# st - simple terminal
# See LICENSE file for copyright and license details.

VERSION = 0.0

PREFIX = /usr/local
MANDIR = $(PREFIX)/share/man

CFLAGS = -DVERSION=\"0.0\" -D_GNU_SOURCE

all: st std

clean:
	rm -f st std
	rm -f st.o std.o
	rm -f st-$(VERSION).tar.gz

dist: clean
	mkdir st-$(VERSION)
	cp -f LICENSE README st-$(VERSION)
	cp -f Makefile config.mk st-$(VERSION)
	cp -f st.1 std.1 st-$(VERSION)
	cp -f st.c std.c st-$(VERSION)
	tar -czf st-$(VERSION).tar st-$(VERSION)
	rm -rf st-$(VERSION)

install:
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f st $(DESTDIR)$(PREFIX)/bin
	cp -f std $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/st
	chmod 755 $(DESTDIR)$(PREFIX)/bin/std
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	sed 's/VERSION/$(VERSION)/g' < st.1 > $(DESTDIR)$(MANDIR)/man1/st.1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/st.1
	sed 's/VERSION/$(VERSION)/g' < std.1 > $(DESTDIR)$(MANDIR)/man1/std.1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/std.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/st
	rm -f $(DESTDIR)$(PREFIX)/bin/std
	rm -f $(DESTDIR)$(MANDIR)/man1/st.1
	rm -f $(DESTDIR)$(MANDIR)/man1/std.1

.PHONY: all clean dist install uninstall
