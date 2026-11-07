#!/usr/bin/env bash
set -euo pipefail
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
ffmpeg -v error -nostdin -f lavfi -i 'color=c=red:s=320x180:r=1:d=2' \
  -f lavfi -i 'color=c=blue:s=320x180:r=1:d=2' \
  -filter_complex '[0:v][1:v]concat=n=2:v=1:a=0' -c:v ffv1 -g 1 "$work/video with spaces.mkv"
export CELLULOID_PREVIEW_TEST_VIDEO="$work/video with spaces.mkv"
# No desktop session or actual media is used by the widget test.
export GSETTINGS_BACKEND=memory GTK_A11Y=none GSK_RENDERER=cairo
if [[ -z ${DISPLAY:-} ]]; then
  xvfb-run -a dbus-run-session -- "$1"
else
  dbus-run-session -- "$1"
fi
