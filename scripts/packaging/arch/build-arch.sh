#!/bin/bash
# Build + smoke the pacman package inside the archlinux container (/work = repo).
#   build-arch.sh build
#   build-arch.sh smoke
# makepkg refuses to run as root -> dedicated builder user on a source copy.
set -eu
VER=$(sed -n 's/^project(bootroll VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

if [ "${1:-build}" = build ]; then
    id builder >/dev/null 2>&1 || useradd -m builder
    rm -rf /tmp/src && mkdir -p /tmp/src
    # Source tarball matching PKGBUILD source=() (same snapshot rule as rpm).
    tar --exclude=.git --exclude=build --exclude=dist \
        -czf /tmp/src/bootroll-${VER}.tar.gz \
        --transform "s,^\.,bootroll-${VER}," .
    # Inject pkgver from project VERSION (single source of truth, same rule as rpm).
    sed "s/^pkgver=.*/pkgver=${VER}/" scripts/packaging/arch/PKGBUILD > /tmp/src/PKGBUILD
    chown -R builder:builder /tmp/src
    su builder -c "cd /tmp/src && makepkg -f --skipinteg --noconfirm > /tmp/makepkg.log 2>&1" \
        || { tail -25 /tmp/makepkg.log; exit 1; }
    mkdir -p dist
    cp /tmp/src/bootroll-${VER}-1-x86_64.pkg.tar.zst dist/ 2>/dev/null \
        || cp /tmp/src/*.pkg.tar.zst dist/
    ls -la dist/*.pkg.tar.zst
    echo "ARCH BUILD OK"
    exit 0
fi

# ---- smoke ----
echo "== install =="
pacman -U --noconfirm "/work/dist/bootroll-${VER}-1-x86_64.pkg.tar.zst" > /tmp/pacman.log 2>&1 \
    || { tail -20 /tmp/pacman.log; exit 1; }
pacman -Qi bootroll | grep -E "^(Name|Version)"

echo "== Xvfb run =="
Xvfb :31 -screen 0 1280x800x24 > /dev/null 2>&1 &
XPID=$!
sleep 1
rm -f /usr/bin/bootroll.log
rc=124
DISPLAY=:31 timeout 8 /usr/bin/bootroll > /tmp/app.log 2>&1 || rc=$?
kill $XPID 2>/dev/null
if [ $rc -ne 124 ]; then
    echo "ARCH SMOKE FAILED: app exited early rc=$rc"
    tail -8 /tmp/app.log
    exit 1
fi
grep -E "boot [0-9]+\.|disks enumerated|font:" /usr/bin/bootroll.log | head -4

echo "== remove =="
pacman -R --noconfirm bootroll > /dev/null 2>&1
[ ! -e /usr/bin/bootroll ] && echo "removed cleanly"
echo "ARCH SMOKE OK"
