%{!?zen_viewers_version:%global zen_viewers_version 0.2.0}
Name:           zen-viewers
Version:        %{zen_viewers_version}
Release:        1%{?dist}
Summary:        Native PDF, image and Markdown viewers
License:        GPL-3.0-or-later
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(gtk4) >= 4.6
BuildRequires:  pkgconfig(poppler-glib)
BuildRequires:  pkgconfig(libcmark)
Requires:       zen-pdf = %{version}-%{release}
Requires:       zen-image = %{version}-%{release}
Requires:       zen-markdown = %{version}-%{release}

%description
A metapackage that installs all three native GTK readers.

%package -n zen-pdf
Summary:        Borderless PDF reader with selectable text
%description -n zen-pdf
A Poppler-based reader with fit-to-width pages and text selection.

%package -n zen-image
Summary:        Borderless image viewer with fit and zoom controls
%description -n zen-image
A GTK image viewer with fit and zoom controls.

%package -n zen-markdown
Summary:        CommonMark reader with selectable text
%description -n zen-markdown
A cmark-based reader with a formatted text view and source mode.

%prep
%setup -q

%build
%make_build

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
%doc apps/zen-markdown/README.md
%{_bindir}/zen-markdown
%{_datadir}/applications/io.github.zenviewers.Markdown.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.zenviewers.Markdown.svg
