# SPDX-License-Identifier: GPL-2.0-only
# Standalone:  nix-shell -p gcc gnumake pkg-config libusb1 --run \
#                "make EVDI_INC=/path/to/evdi/library EVDI_LIB=/path/to/libevdi/dir"
# Nix:         nix build .#wacom-t2d
PREFIX ?= /usr/local
DATADIR ?= $(PREFIX)/share/wacom-t2d
EVDI_INC ?= /usr/include
EVDI_LIB ?= /usr/lib
CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += -I$(EVDI_INC) $(shell pkg-config --cflags libusb-1.0) -DT2D_DATADIR='"$(DATADIR)"'
LDLIBS += -L$(EVDI_LIB) -levdi $(shell pkg-config --libs libusb-1.0)

wacom-t2d: wacom-t2d.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: wacom-t2d
	install -Dm755 wacom-t2d $(DESTDIR)$(PREFIX)/bin/wacom-t2d
	install -Dm644 init.bin $(DESTDIR)$(DATADIR)/init.bin
	install -Dm644 panel_edid.bin $(DESTDIR)$(DATADIR)/panel_edid.bin

clean:
	rm -f wacom-t2d
.PHONY: install clean
