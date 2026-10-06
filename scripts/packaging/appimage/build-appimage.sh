#!/bin/bash
# Build + smoke the AppImage inside the ubuntu22.04 container (/work = repo).
#   build-appimage.sh build
#   build-appimage.sh smoke
# linuxdeploy is fetched at build time; the pinned URL/hash is recorded in
# docs/DISTRO_NOTES.md after the first successful fetch (reproducibility).
set -eu
VER=$(sed -n 's/^project(bootroll VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
LINUXDEPLOY_URL=${LINUXDEPLOY_URL:-https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage}

if [ "${1:-build}" = build ]; then
    cmake -S . -B build/pkg-appimage -DCMAKE_BUILD_TYPE=Release > /tmp/conf.log 2>&1
    cmake --build build/pkg-appimage -j"$(nproc)" > /tmp/build.log 2>&1
    grep -ciE "warning|error" /tmp/build.log | grep -q '^0$' || { echo "BUILD NOT CLEAN"; exit 1; }

    rm -rf /tmp/appdir
    DESTDIR=/tmp/appdir cmake --install build/pkg-appimage --prefix /usr > /dev/null
    # Prune foreign install rules (tinygettext ships its own install()).
    rm -rf /tmp/appdir/usr/lib /tmp/appdir/usr/include

    curl -fsSL -o /tmp/linuxdeploy "$LINUXDEPLOY_URL"
    chmod +x /tmp/linuxdeploy
    echo "linuxdeploy sha256: $(sha256sum /tmp/linuxdeploy | cut -d' ' -f1)"

    export APPIMAGE_EXTRACT_AND_RUN=1
    export VERSION=$VER
    mkdir -p dist
    (cd dist && /tmp/linuxdeploy --appdir /tmp/appdir \
        --executable /tmp/appdir/usr/bin/bootroll \
        --desktop-file /tmp/appdir/usr/share/applications/bootroll.desktop \
        --icon-file /tmp/appdir/usr/share/icons/hicolor/128x128/apps/bootroll.png \
        --output appimage) > /tmp/linuxdeploy.log 2>&1 \
        || { tail -25 /tmp/linuxdeploy.log; exit 1; }
    ls -la dist/*.AppImage
    echo "APPIMAGE BUILD OK"
    exit 0
fi

# ---- smoke (no FUSE in containers) ----
# The squashfs mount is READ-ONLY, so the portable log (exe-relative) cannot
# be written inside an AppImage - assert on the process instead: timeout(1)
# exit 124 = the app was alive the whole window.
APPIMAGE=$(ls /work/dist/Bootroll-*.AppImage | head -1)
echo "== run $APPIMAGE =="
Xvfb :31 -screen 0 1280x800x24 > /dev/null 2>&1 &
XPID=$!
sleep 1
rc=124
APPIMAGE_EXTRACT_AND_RUN=1 DISPLAY=:31 timeout 8 "$APPIMAGE" > /tmp/app.log 2>&1 || rc=$?
kill $XPID 2>/dev/null
if [ $rc -ne 124 ]; then
    echo "APPIMAGE SMOKE FAILED: app exited early rc=$rc"
    tail -8 /tmp/app.log
    exit 1
fi
echo "AppImage alive across 8s: OK"
echo "APPIMAGE SMOKE OK"
