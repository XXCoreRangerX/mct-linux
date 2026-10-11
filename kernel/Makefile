# SPDX-License-Identifier: GPL-2.0-only
# Out-of-tree build:  make KDIR=/lib/modules/$(uname -r)/build
# Nix:                nix build .#mct-kmod
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
