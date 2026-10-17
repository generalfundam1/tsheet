# tsheet: libc only. Optional native X11 clipboard if libX11 headers exist.
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pedantic -D_GNU_SOURCE
LDLIBS  =
PREFIX  ?= /usr/local
DESTDIR ?=
BINDIR  = $(DESTDIR)$(PREFIX)/bin
MANDIR  = $(DESTDIR)$(PREFIX)/share/man/man1

X11_LIBS := $(shell pkg-config --libs x11 2>/dev/null)
ifneq ($(X11_LIBS),)
CFLAGS  += -DHAVE_XLIB $(shell pkg-config --cflags x11 2>/dev/null)
LDLIBS  += $(X11_LIBS)
endif

# the program name
PROG    ?= tsheet

SRC = app.c term.c io.c ui.c sheet.c html.c clip.c graph.c read.c units.c
HDR = tsheet.h config.h

$(PROG): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(PROG)
	install -d $(BINDIR)
	install -m 755 $(PROG) $(BINDIR)/$(PROG)

uninstall:
	rm -f $(BINDIR)/$(PROG)

clean:
	rm -f $(PROG)

help:
	@echo "targets:"
	@echo "  make              build $(PROG)"
	@echo "  make install      install to $(BINDIR) (PREFIX=$(PREFIX), DESTDIR for packaging)"
	@echo "  make uninstall    remove the installed $(PROG)"
	@echo "  make clean        remove the built $(PROG)"
	@echo "  make help         show this list"

.PHONY: install uninstall clean help
