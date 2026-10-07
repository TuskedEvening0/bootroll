#!/bin/bash
# Build the .deb inside the container (/work = repo, runs as root).
#   build-deb.sh build   -> dist/bootroll_<ver>_amd64.deb
#   build-deb.sh smoke   -> install + Xvfb run + assertions + remove + lintian
set -eu
VER=$(sed -n 's/^project(bootroll VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
echo "deb version: $VER"

if [ "${1:-build}" = build ]; then
    # /work is owned by the host user; git's ownership guard would fail the
    # rev-parse that injects BOOTROLL_GIT_HASH (banner showed "unknown").
    git config --system --add safe.directory /work
    cmake -S . -B build/pkg-deb -DCMAKE_BUILD_TYPE=Release > /tmp/conf.log 2>&1
    cmake --build build/pkg-deb -j"$(nproc)" > /tmp/build.log 2>&1
    grep -ciE "warning|error" /tmp/build.log | grep -q '^0$' || { echo "BUILD NOT CLEAN"; grep -iE "warning|error" /tmp/build.log | head; exit 1; }

    rm -rf /tmp/staging
    DESTDIR=/tmp/staging cmake --install build/pkg-deb --prefix /usr > /tmp/install.log
    # Prune foreign install rules (tinygettext ships its own install(): static
    # lib + headers + .pc must not leak into the package).
    rm -rf /tmp/staging/usr/lib /tmp/staging/usr/include
    strip --strip-unneeded /tmp/staging/usr/bin/bootroll
    mkdir -p /tmp/staging/usr/share/doc/bootroll /tmp/staging/DEBIAN
    install -m 0644 LEGAL.md /tmp/staging/usr/share/doc/bootroll/copyright
    INSTALLED_KB=$(du -sk /tmp/staging/usr | cut -f1)
    cat > /tmp/staging/DEBIAN/control <<EOF
Package: bootroll
Version: $VER
Section: utils
Priority: optional
Architecture: amd64
Installed-Size: $INSTALLED_KB
Depends: libc6 (>= 2.35), libstdc++6, libgcc-s1, libglfw3, libgl1
Recommends: zenity, policykit-1, fonts-noto-cjk
Maintainer: bootroll-dev <dev@bootroll.example>
Description: Boot manager utility (BOOTICEx64 clean-room rewrite)
 Boot sector (MBR/PBR), BCD and UEFI boot entry management with a
 hex-based sector editor and bootcode installers.
 .
 License mix: GPL-2.0 (bundled GRUB4DOS bootcode), MIT (imgui),
 GPL (tinygettext), project code (see LEGAL.md).
EOF
    mkdir -p dist
    dpkg-deb --build --root-owner-group /tmp/staging "dist/bootroll_${VER}_amd64.deb" > /tmp/dpkgdeb.log
    dpkg-deb --info "dist/bootroll_${VER}_amd64.deb" | head -12
    echo "DEB BUILD OK"
    exit 0
fi

# ---- smoke ----
echo "== install =="
apt-get install -y -qq --no-install-recommends "/work/dist/bootroll_${VER}_amd64.deb" > /tmp/aptinstall.log 2>&1 || { tail -20 /tmp/aptinstall.log; exit 1; }
dpkg -s bootroll | grep -E "^(Package|Version|Status)"

echo "== desktop-file-validate =="
desktop-file-validate /usr/share/applications/bootroll.desktop && echo "desktop file OK"

echo "== Xvfb run =="
# timeout(1) doubles as the process watchdog (exit 124 = alive the whole
# window) - pkill/procps is not guaranteed in minimal containers. The
# '|| rc=$?' form is REQUIRED: under set -e a failing simple command would
# abort the script before rc is captured.
Xvfb :31 -screen 0 1280x800x24 > /dev/null 2>&1 &
XPID=$!
sleep 1
rm -f /usr/bin/bootroll.log
rc=124
DISPLAY=:31 timeout 8 /usr/bin/bootroll > /tmp/app.log 2>&1 || rc=$?
kill $XPID 2>/dev/null
if [ $rc -ne 124 ]; then
    echo "DEB SMOKE FAILED: app exited early rc=$rc"
    tail -8 /tmp/app.log
    exit 1
fi
grep -E "boot [0-9]+\.|disks enumerated|font:" /usr/bin/bootroll.log | head -4

echo "== remove =="
apt-get remove -y -qq bootroll > /dev/null
[ ! -e /usr/bin/bootroll ] && echo "removed cleanly"
[ ! -e /usr/share/applications/bootroll.desktop ] && echo "desktop file removed"

echo "== lintian =="
lintian --pedantic "/work/dist/bootroll_${VER}_amd64.deb" > "/work/dist/reports/lintian-${VER}.txt" 2>&1 || true
tail -n +1 "/work/dist/reports/lintian-${VER}.txt" | head -20
echo "DEB SMOKE OK"
