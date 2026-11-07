CC ?= cc
AR ?= ar
VIEWERS = zen-pdf zen-image zen-markdown zen-3d zen-video zen-books zen-font zen-table zen-map
APPS ?= $(VIEWERS)
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
FONT_CFLAGS = $(shell pkg-config --cflags fontconfig freetype2 pangoft2)
FONT_LIBS = $(shell pkg-config --libs fontconfig freetype2 pangoft2)
MAP_CFLAGS = $(shell pkg-config --cflags libcurl libxml-2.0 json-glib-1.0)
MAP_LIBS = $(shell pkg-config --libs libcurl libxml-2.0 json-glib-1.0)
TABLE_CFLAGS = -Ibuild/carquet-src/include $(shell pkg-config --cflags libzstd zlib liblz4 libbrotlidec libbrotlienc)
TABLE_LIBS = $(shell pkg-config --libs libzstd zlib liblz4 libbrotlidec libbrotlienc)
CARQUET_ARCHIVE = vendor/carquet/carquet-0.7.3.tar.gz
CARQUET_LIBRARY = build/carquet/libcarquet.a
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
zen-font: build/zen-font
zen-table: build/zen-table
zen-map: build/zen-map

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
build/zen-font: apps/zen-font/src/main.c build/libzen-ui.a common/zen-ui.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_FLAG) $(GTK_CFLAGS) $(FONT_CFLAGS) $(CFLAGS) -Icommon $< build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(FONT_LIBS) -lm
build/carquet-src/.ready: $(CARQUET_ARCHIVE) | build
	@echo 'c4d0077170b64966429aaf8d194095008191b8730c5150185fc6fe884adc5d6d  $(CARQUET_ARCHIVE)' | sha256sum --check --status
	mkdir -p build/carquet-src
	tar -xzf $(CARQUET_ARCHIVE) -C build/carquet-src --strip-components=1
	touch $@
$(CARQUET_LIBRARY): build/carquet-src/.ready vendor/carquet/CMakeLists.txt
	CC="$(CC)" cmake -S vendor/carquet -B build/carquet -DCARQUET_SOURCE_DIR="$(abspath build/carquet-src)" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$(CPPFLAGS) $(CFLAGS)"
	+cmake --build build/carquet --target carquet
TABLE_SRC = $(addprefix apps/zen-table/src/,main.c table-data.c parquet-native.c)
build/zen-table: $(TABLE_SRC) apps/zen-table/src/table-data.h apps/zen-table/src/parquet-native.h $(CARQUET_LIBRARY) build/libzen-ui.a common/zen-ui.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_FLAG) $(GTK_CFLAGS) $(TABLE_CFLAGS) $(CFLAGS) -Icommon $(TABLE_SRC) build/libzen-ui.a $(CARQUET_LIBRARY) -o $@ $(LDFLAGS) $(GTK_LIBS) $(TABLE_LIBS) -lm
MAP_SRC = $(addprefix apps/zen-map/src/,main.c map-data.c tiles.c)
build/zen-map: $(MAP_SRC) apps/zen-map/src/map-data.h apps/zen-map/src/tiles.h build/libzen-ui.a common/zen-ui.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_FLAG) $(GTK_CFLAGS) $(MAP_CFLAGS) $(CFLAGS) -Icommon $(MAP_SRC) build/libzen-ui.a -o $@ $(LDFLAGS) $(GTK_LIBS) $(MAP_LIBS) -lm
build/zen-map-test: apps/zen-map/tests/test-data.c apps/zen-map/src/map-data.c apps/zen-map/src/map-data.h | build
	$(CC) $(CPPFLAGS) $(MAP_CFLAGS) $(CFLAGS) -Iapps/zen-map/src $(filter %.c,$^) -o $@ $(LDFLAGS) $(MAP_LIBS) -lm

check: all build/zen-map-test
	build/zen-pdf --self-test
	build/zen-markdown --self-test
	build/zen-3d --inspect examples/trefoil.glb >/dev/null
	build/zen-video --version
	build/zen-books --inspect examples/above-the-fog.epub >/dev/null
	build/zen-font --version
	build/zen-table --version
	build/zen-map --version
	build/zen-map-test
	sh apps/zen-table/tests/data-smoke.sh build/zen-table
check-data: all
	python3 apps/zen-books/tests/parser-smoke.py
	python3 apps/zen-table/tests/data-smoke.py build/zen-table
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
	python3 apps/zen-font/tests/ui-smoke.py build/zen-font
	python3 apps/zen-table/tests/ui-smoke.py build/zen-table
	xvfb-run -a -s '-screen 0 1280x1024x24' dbus-run-session -- python3 apps/zen-map/tests/ui-smoke.py build/zen-map
	xvfb-run -a -s '-screen 0 1280x1024x24' dbus-run-session -- python3 apps/zen-map/tests/continuity-smoke.py build/zen-map
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
	  if [ "$$app" = zen-table ]; then \
	    rm -f "$(DESTDIR)$(PREFIX)/libexec/zen-viewers/zen-table-parquet"; \
	    install -Dm644 apps/zen-table/data/io.github.zenviewers.Table.xml "$(DESTDIR)$(PREFIX)/share/mime/packages/io.github.zenviewers.Table.xml"; \
	  fi; \
	done
uninstall:
	@set -e; for app in $(APPS); do \
	  rm -f "$(DESTDIR)$(PREFIX)/bin/$$app"; \
	  if [ "$$app" = zen-image ]; then rm -f "$(DESTDIR)$(PREFIX)/share/mime/packages/io.github.zenimage-raw.xml"; fi; \
	  if [ "$$app" = zen-table ]; then rm -f "$(DESTDIR)$(PREFIX)/libexec/zen-viewers/zen-table-parquet" "$(DESTDIR)$(PREFIX)/share/mime/packages/io.github.zenviewers.Table.xml"; fi; \
	  for file in apps/$$app/data/*.desktop; do rm -f "$(DESTDIR)$(PREFIX)/share/applications/$$(basename "$$file")"; done; \
	  for file in apps/$$app/data/*.svg; do rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$$(basename "$$file")"; done; \
	done

deb:
	./scripts/build-deb.sh
rpm:
	./scripts/build-rpm.sh
clean:
	rm -rf build
.PHONY: all $(VIEWERS) check check-data check-ui install uninstall deb rpm clean FORCE_IMAGE_FEATURES
