# Celluloid Preview

A small fork of Celluloid 0.30 that keeps its GTK interface and adds video
thumbnails to the timestamp popup when hovering over the progress bar.

The feature works with the normal, compact and fullscreen controls. Playback
continues normally while a separate FFmpeg process decodes a preview frame.
Hovering never seeks the playback instance.

## How previews work

- Hover requests start after 40 ms and use nearby keyframes for quick previews.
- Each seek bar keeps an LRU cache of up to 64 small frames for the current file.
- Moving updates the latest requested position without repeatedly restarting the
  decoder. The previous thumbnail remains visible while the next one loads.
  Leaving the seek bar, changing files or closing cancels pending work.
- Decoding uses one thread and is killed after eight seconds if it stalls.
- Network streams, audio-only files, unavailable codecs and decoding failures
  retain the original timestamp popup. No thumbnails are written to disk.

Local videos must be decodable by the installed FFmpeg build. Fedora's
`ffmpeg-free` has a more limited codec selection than some third-party builds.
This fork does not change repositories or install codec replacements.

## Fedora RPM build

Install Podman, then run:

```bash
tools/build-rpm.sh
```

The Fedora 44 container builds the application and runs the upstream tests plus
real thumbnail extraction and lifecycle tests. RPMs are written under
`build-output/RPMS/`. The source RPM is under `build-output/SRPMS/`.

The RPM retains the `celluloid` package/application identity and uses Epoch 1
so Fedora's unpatched package does not silently replace the preview fork.
Installing it replaces the packaged Celluloid executable; no preferences or
playlists are erased. To return to Fedora's package, use DNF distro-sync with
only Fedora repositories enabled.

## Build from source

With Celluloid's normal GTK 4, libadwaita and libmpv development dependencies,
plus FFmpeg and Xvfb for the preview test:

```bash
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
build/src/celluloid --new-window /path/to/video.mkv
```

This is a preview fork, not an upstream Celluloid release. The `upstream` Git
remote points to the original project. No Strata integration or system
installation is performed by checking out this repository.
