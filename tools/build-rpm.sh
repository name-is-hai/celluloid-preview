#!/usr/bin/env bash
# Build the preview fork in Fedora, without adding build dependencies to the host.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=${CELLULOID_PREVIEW_BUILD_OUTPUT:-$root/build-output}
mkdir -p "$output/SOURCES"
version=$(awk '/^Version:/ { print $2; exit }' "$root/packaging/celluloid.spec")
tar -C "$root" --exclude=./.git --exclude=./build --exclude=./build-output \
  --exclude=./_build --exclude='*.log' --transform="s,^\.,celluloid-preview-$version," \
  -czf "$output/SOURCES/celluloid-preview-$version.tar.gz" .
podman run --rm --user 0 --cpus "${CELLULOID_PREVIEW_BUILD_CPUS:-2}" \
  -v "$root:/src:ro,Z" -v "$output:/out:Z" \
  -v celluloid-preview-dnf-cache:/var/cache/libdnf5:Z \
  registry.fedoraproject.org/fedora:44 bash -euo pipefail -c '
    dnf install -y --setopt=install_weak_deps=False rpm-build dnf-plugins-core
    dnf builddep -y --setopt=install_weak_deps=False /src/packaging/celluloid.spec
    rpmbuild -ba /src/packaging/celluloid.spec --define "_topdir /out" \
      --define "_smp_build_ncpus 2"
  '
printf 'RPMs: %s/RPMS\n' "$output"
