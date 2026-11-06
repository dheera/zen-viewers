%{!?zen_viewers_version:%global zen_viewers_version 0.5.0}
%global cmark_gfm_version 0.29.0.gfm.13
# Downstream packagers can use rpmbuild --without raw for a lighter image viewer.
%bcond_without raw
%if %{with raw}
%global image_raw yes
%else
%global image_raw no
%endif
Name:           zen-viewers
Version:        %{zen_viewers_version}
Release:        1%{?dist}
Summary:        Native PDF, image, Markdown, 3D, video and book viewers
License:        GPL-3.0-or-later
Source0:        %{name}-%{version}.tar.gz
# https://codeload.github.com/github/cmark-gfm/tar.gz/refs/tags/0.29.0.gfm.13
Source1:        cmark-gfm-%{cmark_gfm_version}.tar.gz
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  cmake
BuildRequires:  python3
BuildRequires:  pkgconfig(gtk4) >= 4.6
BuildRequires:  pkgconfig(poppler-glib)
BuildRequires:  pkgconfig(libexif)
%if %{with raw}
BuildRequires:  pkgconfig(libraw_r) >= 0.20
%endif
BuildRequires:  pkgconfig(epoxy)
BuildRequires:  pkgconfig(assimp)
BuildRequires:  pkgconfig(mpv)
BuildRequires:  pkgconfig(libarchive)
BuildRequires:  pkgconfig(libxml-2.0)
Requires:       zen-pdf = %{version}-%{release}
Requires:       zen-image = %{version}-%{release}
Requires:       zen-markdown = %{version}-%{release}
Requires:       zen-3d = %{version}-%{release}
Requires:       zen-video = %{version}-%{release}
Requires:       zen-books = %{version}-%{release}

%description
A metapackage that installs all six native GTK viewers.

%package -n zen-pdf
Summary:        Borderless PDF reader with smooth scrolling and selectable text
%description -n zen-pdf
A Poppler-based reader with fit-width pages, smooth scrolling, a document outline and text selection.

%package -n zen-image
Summary:        Borderless image viewer with folder navigation and metadata
%description -n zen-image
A GTK image viewer with folder navigation, metadata, and fit and zoom controls.

%package -n zen-markdown
Summary:        Markdown reader with formatted tables and selectable text
License:        GPL-3.0-or-later AND BSD-2-Clause AND MIT
Provides:       bundled(cmark-gfm) = %{cmark_gfm_version}
%description -n zen-markdown
A cmark-gfm reader with native formatted tables, selectable text and source mode.

%package -n zen-3d
Summary:        Borderless 3D model viewer with interactive orbit and zoom
%description -n zen-3d
A GTK and OpenGL viewer for local 3D models, using Assimp for file import.

%package -n zen-video
Summary:        Borderless video player with native playback controls
%description -n zen-video
A GTK and libmpv video player with seek, volume and fullscreen controls.

%package -n zen-books
Summary:        Borderless EPUB and CBZ reader with chapter navigation
%description -n zen-books
A native GTK EPUB and CBZ reader with selectable text and chapter navigation.

%prep
%setup -q
echo '5abc61798ebd9de5660bc076443c07abad2b8d15dbc11094a3a79644b8ad243a  %{SOURCE1}' | sha256sum -c -
tar -xzf %{SOURCE1}

%build
%set_build_flags
cmake -S cmark-gfm-%{cmark_gfm_version} -B cmark-build \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
    -DCMARK_SHARED=OFF -DCMARK_STATIC=ON -DCMARK_TESTS=OFF \
    -DCMAKE_C_FLAGS="$CFLAGS"
cmake --build cmark-build -- %{?_smp_mflags}
%make_build CFLAGS="%{build_cflags}" LDFLAGS="%{build_ldflags}" IMAGE_RAW=%{image_raw} \
    MARKDOWN_CFLAGS="-Icmark-gfm-%{cmark_gfm_version}/src -Icmark-gfm-%{cmark_gfm_version}/extensions -Icmark-build/src -Icmark-build/extensions -DCMARK_GFM_STATIC_DEFINE -DCMARK_GFM_EXTENSIONS_STATIC_DEFINE" \
    MARKDOWN_LIBS="cmark-build/extensions/libcmark-gfm-extensions.a cmark-build/src/libcmark-gfm.a"

%check
%{__make} check IMAGE_RAW=%{image_raw}

%install
%{__make} install PREFIX=%{_prefix} DESTDIR=%{buildroot} IMAGE_RAW=%{image_raw}

%files
%license LICENSE

%files -n zen-pdf
%license LICENSE
%doc apps/zen-pdf/README.md
%{_bindir}/zen-pdf
%{_datadir}/applications/io.github.zenpdf.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.zenpdf.svg

%files -n zen-image
%license LICENSE
%doc apps/zen-image/README.md
%{_bindir}/zen-image
%{_datadir}/applications/io.github.zenimage.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.zenimage.svg
%if %{with raw}
%{_datadir}/mime/packages/io.github.zenimage-raw.xml
%endif

%files -n zen-markdown
%license LICENSE
%license cmark-gfm-%{cmark_gfm_version}/COPYING
%doc apps/zen-markdown/README.md
%{_bindir}/zen-markdown
%{_datadir}/applications/io.github.zenviewers.Markdown.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.zenviewers.Markdown.svg

%files -n zen-3d
%license LICENSE
%doc apps/zen-3d/README.md
%{_bindir}/zen-3d
%{_datadir}/applications/io.github.zenviewers.ThreeD.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.zenviewers.ThreeD.svg

%files -n zen-video
%license LICENSE
%doc apps/zen-video/README.md
%{_bindir}/zen-video
%{_datadir}/applications/io.github.zenviewers.Video.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.zenviewers.Video.svg

%files -n zen-books
%license LICENSE
%doc apps/zen-books/README.md
%{_bindir}/zen-books
%{_datadir}/applications/io.github.zenviewers.Books.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.zenviewers.Books.svg
