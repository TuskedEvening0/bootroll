#!/bin/bash
# Portable tar.gz: floor-glibc binary + license. Runs in the appimage container.
set -eu
VER=$(sed -n 's/^project(bootroll VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
mkdir -p dist/payload/bootroll-"$VER"
cp build/pkg-appimage/bin/bootroll dist/payload/bootroll-"$VER"/bootroll
cp LEGAL.md dist/payload/bootroll-"$VER"/LICENSE
tar -czf dist/bootroll-"$VER"-linux-x86_64.tar.gz -C dist/payload "bootroll-$VER"
rm -rf dist/payload
tar -tzf dist/bootroll-"$VER"-linux-x86_64.tar.gz
echo "TARGZ OK"
