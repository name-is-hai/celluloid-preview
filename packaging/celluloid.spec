Name:           celluloid
Epoch:          1
Version:        0.30
Release:        4%{?dist}
Summary:        GTK video player with thumbnail previews over the progress bar
License:        GPL-3.0-or-later
URL:            https://github.com/name-is-hai/celluloid-preview
Source0:        celluloid-preview-%{version}.tar.gz
BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(gtk4) >= 4.16
BuildRequires:  pkgconfig(libadwaita-1) >= 1.8
BuildRequires:  pkgconfig(mpv) >= 1.107
BuildRequires:  pkgconfig(epoxy)
BuildRequires:  gettext-devel
BuildRequires:  desktop-file-utils
BuildRequires:  ffmpeg-free
BuildRequires:  xorg-x11-server-Xvfb
BuildRequires:  xorg-x11-xauth
BuildRequires:  dbus-daemon
Requires:       ffmpeg-free

%description
Celluloid's existing GTK interface, extended with on-demand local-video
thumbnail previews in its progress-bar timestamp popup. Preview decoding runs
separately from playback and stops when the pointer leaves the progress bar.

%prep
%autosetup -n celluloid-preview-%{version}

%build
%meson
%meson_build

%install
%meson_install
%find_lang %{name}

%check
%meson_test

%files -f %{name}.lang
%license COPYING
%doc README.md README-PREVIEW.md AUTHORS
%{_bindir}/celluloid
%{_datadir}/applications/io.github.celluloid_player.Celluloid.desktop
%{_datadir}/dbus-1/services/io.github.celluloid_player.Celluloid.service
%{_datadir}/glib-2.0/schemas/io.github.celluloid_player.Celluloid.gschema.xml
%{_datadir}/icons/hicolor/scalable/apps/io.github.celluloid_player.Celluloid.svg
%{_datadir}/icons/hicolor/symbolic/apps/io.github.celluloid_player.Celluloid-symbolic.svg
%{_datadir}/metainfo/io.github.celluloid_player.Celluloid.appdata.xml
%{_mandir}/man1/celluloid.1*

%changelog
* Sat Nov 07 2026 Celluloid Preview Maintainers - 1:0.30-4
- Publish independently built and tested Fedora RPM releases for Strata to import

* Sat Nov 07 2026 Celluloid Preview Maintainers - 1:0.30-2.preview2
- Update thumbnails while moving instead of restarting the decoder
- Use fast keyframe previews, an LRU cache and a compact popup frame

* Sat Nov 07 2026 Celluloid Preview Maintainers - 1:0.30-2.preview1
- Add asynchronous, cancellable thumbnail previews to the native progress bar
- Test actual frame extraction, caching, file changes and widget destruction
