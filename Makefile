CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lsqlite3 -pthread

# Native X11 selection ownership also works through XWayland in typical KDE/GNOME
# Wayland sessions. No wl-copy, xclip, or xsel executable is used at runtime.
ifneq ($(shell pkg-config --exists x11 && echo yes),)
CPPFLAGS += -DHAVE_X11 $(shell pkg-config --cflags x11)
LDLIBS += $(shell pkg-config --libs x11)
endif

.PHONY: all clean install test

all: lamepaste

lamepaste: lamepaste.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 $(LDFLAGS) -o $@ $< $(LDLIBS)

install: lamepaste
	install -Dm755 lamepaste "$(DESTDIR)$(HOME)/.local/bin/lamepaste"

test: lamepaste
	bash tests/test_lamepaste.sh
	python3 tests/test_progress.py ./lamepaste

clean:
	rm -f lamepaste
