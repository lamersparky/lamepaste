CXX ?= c++
CXXFLAGS ?= -O2 -pipe -fstack-protector-strong -D_FORTIFY_SOURCE=3 \
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lsqlite3 -pthread

# Native X11 clipboard ownership also works through XWayland in typical KDE/GNOME
# Wayland sessions. No wl-copy, xclip, or xsel executable is used at runtime.
ifneq ($(shell pkg-config --exists x11 && echo yes),)
CPPFLAGS += -DHAVE_X11 $(shell pkg-config --cflags x11)
LDLIBS += $(shell pkg-config --libs x11)
endif

.PHONY: all clean install test test-sanitize

all: lamepaste

lamepaste: lamepaste.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 $(LDFLAGS) -o $@ $< $(LDLIBS)

install: lamepaste
	install -Dm755 lamepaste "$(DESTDIR)$(HOME)/.local/bin/lamepaste"

test: lamepaste
	bash tests/test_lamepaste.sh
	python3 tests/test_progress.py ./lamepaste

test-sanitize: clean
	$(MAKE) CXXFLAGS='-O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' lamepaste
	ASAN_OPTIONS=detect_leaks=0 bash tests/test_lamepaste.sh
	ASAN_OPTIONS=detect_leaks=0 python3 tests/test_progress.py ./lamepaste

clean:
	rm -f lamepaste
