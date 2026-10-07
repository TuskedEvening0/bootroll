#!/bin/bash
# Build + smoke the .rpm inside a fedora/opensuse container (/work = repo).
#   build-rpm.sh build <distro-tag>
#   build-rpm.sh smoke <distro-tag>
set -eu
VER=$(sed -n 's/^project(bootroll VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
DISTDIR=/root/rpmbuild
# Distro label for report filenames (fedora / opensuse-tumbleweed).
TAG=$(. /etc/os-release && echo "$ID")

if [ "${1:-build}" = build ]; then
    mkdir -p "$DISTDIR"/{BUILD,RPMS,SOURCES,SPECS,SRPMS} dist/reports
    # Source snapshot: everything except VCS/build outputs.
    tar --exclude=.git --exclude=build --exclude=dist \
        -czf "$DISTDIR/SOURCES/bootroll-${VER}.tar.gz" \
        --transform "s,^\.,bootroll-${VER}," .
    sed "s/^Version:.*/Version:        ${VER}/" scripts/packaging/rpm/bootroll.spec \
        > "$DISTDIR/SPECS/bootroll.spec"
    rpmbuild -bb --define "_topdir $DISTDIR" "$DISTDIR/SPECS/bootroll.spec" \
        > /tmp/rpmbuild.log 2>&1 || { tail -25 /tmp/rpmbuild.log; exit 1; }
    find "$DISTDIR/RPMS" -name '*.rpm' -exec cp {} dist/ \;
    ls -la dist/*.rpm
    echo "RPM BUILD OK ($TAG)"
    exit 0
fi

# ---- smoke ----
# Pick THIS distro's rpm (dist/ may hold several families).
if [ "$TAG" = fedora ]; then
    RPM=$(ls /work/dist/bootroll-${VER}-*fc*.x86_64.rpm | head -1)
else
    RPM=$(ls /work/dist/bootroll-${VER}-*.x86_64.rpm | grep -vE "fc|debug" | head -1)
fi
[ -n "$RPM" ] || { echo "no rpm found for $TAG"; exit 1; }
echo "== install $RPM =="
# NOTE: 'which' is absent in minimal Fedora images - use the bash builtin.
if command -v dnf >/dev/null 2>&1; then
    dnf -y install "$RPM" > /tmp/dnf.log 2>&1 || { tail -20 /tmp/dnf.log; exit 1; }
    PKGCMD="dnf remove -y bootroll"
else
    zypper --non-interactive --gpg-auto-import-keys refresh > /dev/null 2>&1
    zypper --non-interactive --no-gpg-checks install "$RPM" > /tmp/zy.log 2>&1 || { tail -20 /tmp/zy.log; exit 1; }
    PKGCMD="zypper --non-interactive remove bootroll"
fi
rpm -q bootroll

echo "== Xvfb run =="
Xvfb :31 -screen 0 1280x800x24 > /dev/null 2>&1 &
XPID=$!
sleep 1
rm -f /usr/bin/bootroll.log
rc=124
DISPLAY=:31 timeout 8 /usr/bin/bootroll > /tmp/app.log 2>&1 || rc=$?
kill $XPID 2>/dev/null
if [ $rc -ne 124 ]; then
    echo "RPM SMOKE FAILED: app exited early rc=$rc"
    tail -8 /tmp/app.log
    exit 1
fi
grep -E "boot [0-9]+\.|disks enumerated|font:" /usr/bin/bootroll.log | head -4

echo "== rpmlint =="
rpmlint "$RPM" > "/work/dist/reports/rpmlint-${TAG}-${VER}.txt" 2>&1 || true
cat "/work/dist/reports/rpmlint-${TAG}-${VER}.txt" | head -15

echo "== remove =="
$PKGCMD > /dev/null 2>&1
[ ! -e /usr/bin/bootroll ] && echo "removed cleanly"
echo "RPM SMOKE OK ($TAG)"
