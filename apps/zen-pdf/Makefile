# Compatibility entry point; build and packaging live at the repository root.
ROOT = ../..
PREFIX ?= /usr/local
all:
	$(MAKE) -C $(ROOT) zen-pdf
	ln -sf ../../build/zen-pdf zen-pdf
check: all
	./zen-pdf --self-test
install:
	$(MAKE) -C $(ROOT) APPS=zen-pdf PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" install
uninstall:
	$(MAKE) -C $(ROOT) APPS=zen-pdf PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" uninstall
deb:
	./scripts/build-deb.sh
rpm:
	./scripts/build-rpm.sh
clean:
	rm -f zen-pdf
.PHONY: all check install uninstall deb rpm clean
