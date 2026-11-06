CC ?= cc
AR ?= ar
APPS ?= zen-pdf zen-image zen-markdown zen-3d zen-video zen-books
PREFIX ?= /usr/local
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wformat=2 -Wstrict-prototypes
GTK_CFLAGS = $(shell pkg-config --cflags gtk4)
GTK_LIBS = $(shell pkg-config --libs gtk4)
PDF_CFLAGS = $(shell pkg-config --cflags poppler-glib)
PDF_LIBS = $(shell pkg-config --libs poppler-glib)
MARKDOWN_CFLAGS = $(shell pkg-config --cflags libcmark-gfm)
MARKDOWN_LIBS = $(shell pkg-config --libs libcmark-gfm)
THREED_CFLAGS = $(shell pkg-config --cflags epoxy assimp)
THREED_LIBS = $(shell pkg-config --libs epoxy assimp)
VIDEO_CFLAGS ?= $(shell pkg-config --cflags mpv epoxy)
VIDEO_LIBS ?= $(shell pkg-config --libs mpv epoxy)
BOOKS_CFLAGS = $(shell pkg-config --cflags libarchive libxml-2.0)
BOOKS_LIBS = $(shell pkg-config --libs libarchive libxml-2.0)
IMAGE_EXIF ?= $(shell pkg-config --exists libexif && echo yes)
IMAGE_RAW ?= $(shell pkg-config --exists 'libraw_r >= 0.20' && echo yes || echo no)
IMAGE_RAW_MIME = image/x-canon-cr2;image/x-canon-cr3;image/x-sony-arw;image/x-adobe-dng;image/x-nikon-nef;image/x-fuji-raf;image/x-olympus-orf;image/x-panasonic-rw2;image/x-pentax-pef;
ifeq ($(IMAGE_EXIF),yes)
IMAGE_CFLAGS = $(shell pkg-config --cflags libexif) -DHAVE_LIBEXIF
IMAGE_LIBS = $(shell pkg-config --libs libexif)
endif
ifeq ($(IMAGE_RAW),yes)
IMAGE_CFLAGS += $(shell pkg-config --cflags libraw_r) -DHAVE_LIBRAW
IMAGE_LIBS += $(shell pkg-config --libs libraw_r)
endif
BINARIES = $(addprefix build/,$(APPS))
COMMON = common/zen-ui.c common/zen-ui.h
VERSION_FLAG = -DZEN_VERSION='"$(shell cat VERSION)"'

all: $(BINARIES)
zen-pdf: build/zen-pdf
zen-image: build/zen-image
zen-markdown: build/zen-markdown
zen-3d: build/zen-3d
zen-video: build/zen-video
zen-books: build/zen-books

build:
	mkdir -p build
build/zen-ui.o: $(COMMON) | build
	$(CC) $(CPPFLAGS) $(GTK_CFLAGS) $(CFLAGS) -Icommon -c common/zen-ui.c -o $@
build/libzen-ui.a: build/zen-ui.o
	$(AR) rcs $@ $<
build/zen-pdf: apps/zen-pdf/src/main.c apps/zen-pdf/src/selection.inc apps/zen-pdf/src/view.inc build/libzen-ui.a common/zen-ui.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_FLAG) $(GTK_CFLAGS) $(PDF_CFLAGS) $(CFLAGS) -Icommon $< build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(PDF_LIBS) -lm
# Keep feature switches effective even when a differently configured binary exists.
build/image-features: FORCE_IMAGE_FEATURES | build
	@printf '%s\n' 'exif=$(if $(filter yes,$(IMAGE_EXIF)),yes,no)' 'raw=$(if $(filter yes,$(IMAGE_RAW)),yes,no)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
FORCE_IMAGE_FEATURES:
build/zen-image: apps/zen-image/main.c apps/zen-image/thumbnail.h apps/zen-image/raw.h build/image-features build/libzen-ui.a common/zen-ui.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_FLAG) $(GTK_CFLAGS) $(IMAGE_CFLAGS) $(CFLAGS) -Icommon $< build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(IMAGE_LIBS) -lm
build/zen-markdown: apps/zen-markdown/src/main.c apps/zen-markdown/src/document.c apps/zen-markdown/src/document.h build/libzen-ui.a common/zen-ui.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_FLAG) $(GTK_CFLAGS) $(MARKDOWN_CFLAGS) $(CFLAGS) -Icommon apps/zen-markdown/src/main.c apps/zen-markdown/src/document.c build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(MARKDOWN_LIBS) -lm
ZEN3D_SRC = $(addprefix apps/zen-3d/src/,main.c model.c ply.c renderer.c)
build/zen-3d: $(ZEN3D_SRC) apps/zen-3d/src/model.h apps/zen-3d/src/ply.h apps/zen-3d/src/renderer.h build/libzen-ui.a common/zen-ui.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_FLAG) $(GTK_CFLAGS) $(THREED_CFLAGS) $(CFLAGS) -Icommon $(ZEN3D_SRC) build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(THREED_LIBS) -lm

build/zen-video: apps/zen-video/src/main.c build/libzen-ui.a common/zen-ui.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_FLAG) $(GTK_CFLAGS) $(VIDEO_CFLAGS) $(CFLAGS) -Icommon $< build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(VIDEO_LIBS) -lm
BOOKS_SRC = apps/zen-books/src/main.c apps/zen-books/src/book.c
build/zen-books: $(BOOKS_SRC) apps/zen-books/src/book.h build/libzen-ui.a common/zen-ui.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_FLAG) $(GTK_CFLAGS) $(BOOKS_CFLAGS) $(CFLAGS) -Icommon $(BOOKS_SRC) build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(BOOKS_LIBS) -lm

check: all
	build/zen-pdf --self-test
	build/zen-markdown --self-test
	build/zen-3d --inspect examples/trefoil.glb >/dev/null
	build/zen-video --version
	build/zen-books --inspect examples/above-the-fog.epub >/dev/null
	python3 apps/zen-books/tests/parser-smoke.py
check-ui: all
	python3 tests/chrome-smoke.py build/zen-image
	ZEN_PDF_BIN=$(CURDIR)/build/zen-pdf apps/zen-pdf/tests/ui-smoke.sh
	ZEN_PDF_BIN=$(CURDIR)/build/zen-pdf python3 apps/zen-pdf/tests/motion.py
	ZEN_PDF_BIN=$(CURDIR)/build/zen-pdf python3 apps/zen-pdf/tests/text-selection.py
	python3 apps/zen-image/tests/ui-smoke.py build/zen-image
	bash apps/zen-markdown/tests/reading-smoke.sh build/zen-markdown
	bash apps/zen-markdown/tests/table-smoke.sh build/zen-markdown
	python3 apps/zen-3d/tests/ply-smoke.py
	python3 apps/zen-3d/tests/ui-smoke.py build/zen-3d
	python3 apps/zen-books/tests/ui-smoke.py build/zen-books
	python3 apps/zen-video/tests/ui-smoke.py build/zen-video
install: $(BINARIES)
	@set -e; for app in $(APPS); do \
	  install -Dm755 build/$$app "$(DESTDIR)$(PREFIX)/bin/$$app"; \
	  for file in apps/$$app/data/*.desktop; do \
	    desktop="$(DESTDIR)$(PREFIX)/share/applications/$$(basename "$$file")"; \
	    install -Dm644 "$$file" "$$desktop"; \
	    if [ "$$app" = zen-image ] && [ "$(IMAGE_RAW)" = yes ]; then sed -i '/^MimeType=/s|$$|$(IMAGE_RAW_MIME)|' "$$desktop"; fi; \
	  done; \
	  for file in apps/$$app/data/*.svg; do install -Dm644 "$$file" "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$$(basename "$$file")"; done; \
	  if [ "$$app" = zen-image ] && [ "$(IMAGE_RAW)" = yes ]; then \
	    install -Dm644 apps/zen-image/data/io.github.zenimage-raw.xml "$(DESTDIR)$(PREFIX)/share/mime/packages/io.github.zenimage-raw.xml"; \
	  fi; \
	done
uninstall:
	@set -e; for app in $(APPS); do \
	  rm -f "$(DESTDIR)$(PREFIX)/bin/$$app"; \
	  if [ "$$app" = zen-image ]; then rm -f "$(DESTDIR)$(PREFIX)/share/mime/packages/io.github.zenimage-raw.xml"; fi; \
	  for file in apps/$$app/data/*.desktop; do rm -f "$(DESTDIR)$(PREFIX)/share/applications/$$(basename "$$file")"; done; \
	  for file in apps/$$app/data/*.svg; do rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$$(basename "$$file")"; done; \
	done

deb:
	./scripts/build-deb.sh
rpm:
	./scripts/build-rpm.sh
clean:
	rm -rf build
.PHONY: all zen-pdf zen-image zen-markdown zen-3d zen-video zen-books check check-ui install uninstall deb rpm clean FORCE_IMAGE_FEATURES
