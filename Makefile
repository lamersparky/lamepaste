CC       = cc
CFLAGS   = -O2 -Wall -Wextra -Wpedantic
CPPFLAGS =
LDLIBS   = -lsqlite3

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

.PHONY: all clean install uninstall

all: lamepaste

lamepaste: lamepaste.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: lamepaste
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 lamepaste "$(DESTDIR)$(BINDIR)/lamepaste"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/lamepaste"

clean:
	rm -f lamepaste
