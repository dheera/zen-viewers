%{!?zen_viewers_version:%global zen_viewers_version 0.3.0}
%global cmark_gfm_version 0.29.0.gfm.13
Name:           zen-viewers
Version:        %{zen_viewers_version}
Release:        1%{?dist}
Summary:        Native PDF, image and Markdown viewers
License:        GPL-3.0-or-later
Source0:        %{name}-%{version}.tar.gz
# https://codeload.github.com/github/cmark-gfm/tar.gz/refs/tags/0.29.0.gfm.13
Source1:        cmark-gfm-%{cmark_gfm_version}.tar.gz
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  cmake
BuildRequires:  pkgconfig(gtk4) >= 4.6
BuildRequires:  pkgconfig(poppler-glib)
BuildRequires:  pkgconfig(libexif)
Requires:       zen-pdf = %{version}-%{release}
Requires:       zen-image = %{version}-%{release}
Requires:       zen-markdown = %{version}-%{release}

%description
A metapackage that installs all three native GTK readers.

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
%make_build CFLAGS="%{build_cflags}" LDFLAGS="%{build_ldflags}" \
    MARKDOWN_CFLAGS="-Icmark-gfm-%{cmark_gfm_version}/src -Icmark-gfm-%{cmark_gfm_version}/extensions -Icmark-build/src -Icmark-build/extensions -DCMARK_GFM_STATIC_DEFINE -DCMARK_GFM_EXTENSIONS_STATIC_DEFINE" \
    MARKDOWN_LIBS="cmark-build/extensions/libcmark-gfm-extensions.a cmark-build/src/libcmark-gfm.a"

%check
%{__make} check

%install
%{__make} install PREFIX=%{_prefix} DESTDIR=%{buildroot}

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

%files -n zen-markdown
%license LICENSE
%license cmark-gfm-%{cmark_gfm_version}/COPYING
%doc apps/zen-markdown/README.md
%{_bindir}/zen-markdown
%{_datadir}/applications/io.github.zenviewers.Markdown.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.zenviewers.Markdown.svg
