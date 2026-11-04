CC ?= cc
AR ?= ar
APPS ?= zen-pdf zen-image zen-markdown
PREFIX ?= /usr/local
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wformat=2 -Wstrict-prototypes
GTK_CFLAGS = $(shell pkg-config --cflags gtk4)
GTK_LIBS = $(shell pkg-config --libs gtk4)
PDF_CFLAGS = $(shell pkg-config --cflags poppler-glib)
PDF_LIBS = $(shell pkg-config --libs poppler-glib)
MARKDOWN_CFLAGS = $(shell pkg-config --cflags libcmark)
MARKDOWN_LIBS = $(shell pkg-config --libs libcmark)
BINARIES = $(addprefix build/,$(APPS))
COMMON = common/zen-ui.c common/zen-ui.h

all: $(BINARIES)
zen-pdf: build/zen-pdf
zen-image: build/zen-image
zen-markdown: build/zen-markdown

build:
	mkdir -p build
build/zen-ui.o: $(COMMON) | build
	$(CC) $(CPPFLAGS) $(GTK_CFLAGS) $(CFLAGS) -Icommon -c common/zen-ui.c -o $@
build/libzen-ui.a: build/zen-ui.o
	$(AR) rcs $@ $<
build/zen-pdf: apps/zen-pdf/src/main.c apps/zen-pdf/src/selection.inc build/libzen-ui.a common/zen-ui.h
	$(CC) $(CPPFLAGS) $(GTK_CFLAGS) $(PDF_CFLAGS) $(CFLAGS) -Icommon $< build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(PDF_LIBS) -lm
build/zen-image: apps/zen-image/main.c build/libzen-ui.a common/zen-ui.h
	$(CC) $(CPPFLAGS) $(GTK_CFLAGS) $(CFLAGS) -Icommon $< build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) -lm
build/zen-markdown: apps/zen-markdown/src/main.c apps/zen-markdown/src/document.c apps/zen-markdown/src/document.h build/libzen-ui.a common/zen-ui.h
	$(CC) $(CPPFLAGS) $(GTK_CFLAGS) $(MARKDOWN_CFLAGS) $(CFLAGS) -Icommon apps/zen-markdown/src/main.c apps/zen-markdown/src/document.c build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(MARKDOWN_LIBS) -lm

check: all
	build/zen-pdf --self-test
	build/zen-markdown --self-test
check-ui: all
	ZEN_PDF_BIN=$(CURDIR)/build/zen-pdf apps/zen-pdf/tests/ui-smoke.sh
	ZEN_PDF_BIN=$(CURDIR)/build/zen-pdf python3 apps/zen-pdf/tests/text-selection.py
	python3 apps/zen-image/tests/ui-smoke.py build/zen-image
	bash apps/zen-markdown/tests/reading-smoke.sh build/zen-markdown
install: $(BINARIES)
	@set -e; for app in $(APPS); do \
	  install -Dm755 build/$$app "$(DESTDIR)$(PREFIX)/bin/$$app"; \
	  for file in apps/$$app/data/*.desktop; do install -Dm644 "$$file" "$(DESTDIR)$(PREFIX)/share/applications/$$(basename "$$file")"; done; \
	  for file in apps/$$app/data/*.svg; do install -Dm644 "$$file" "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$$(basename "$$file")"; done; \
	done
uninstall:
	@set -e; for app in $(APPS); do \
	  rm -f "$(DESTDIR)$(PREFIX)/bin/$$app"; \
	  for file in apps/$$app/data/*.desktop; do rm -f "$(DESTDIR)$(PREFIX)/share/applications/$$(basename "$$file")"; done; \
	  for file in apps/$$app/data/*.svg; do rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$$(basename "$$file")"; done; \
	done

deb:
	./scripts/build-deb.sh
rpm:
	./scripts/build-rpm.sh
clean:
	rm -rf build
.PHONY: all zen-pdf zen-image zen-markdown check check-ui install uninstall deb rpm clean
