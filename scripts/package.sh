#!/bin/bash
# M10 packaging orchestrator: five artifact families, all built in their own
# distro containers (ROADMAP decision #3), outputs land in dist/ plus
# SHA256SUMS. Usage: scripts/package.sh [deb|rpm|arch|appimage|targz|all]
set -u
cd "$(dirname "$0")/.." || exit 1
VER=$(sed -n 's/^project(bootroll VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
STAGES=${*:-all}
failed=0

have() { docker image inspect "$1" >/dev/null 2>&1; }
chown_back() { # container writes files as root; hand them back to the user
    docker run --rm -v "$PWD:/work" -e U="$(id -u)" -e G="$(id -g)" "$1" \
        bash -c 'chown -R "$U:$G" /work/dist /work/build/pkg-* 2>/dev/null' || true
}

build_image() { # build_image <path> <tag>
    have "$2" || docker build -f "$1" -t "$2" "$(dirname "$1")" > /tmp/img-$2.log 2>&1 \
        || { echo "IMAGE BUILD FAILED: $2"; tail -20 /tmp/img-$2.log; return 1; }
}

run_stage() { # run_stage <name> <image> <script>
    echo "=== package: $1 ==="
    docker run --rm -v "$PWD:/work" -w /work "$2" bash "$3" || { failed=1; return; }
    chown_back "$2"
}

mkdir -p dist/reports

case "$STAGES" in
    *deb*|*all*)
        build_image scripts/packaging/deb/Dockerfile bootroll-packaging:deb \
            && run_stage deb bootroll-packaging:deb scripts/packaging/deb/build-deb.sh
        docker run --rm -v "$PWD:/work" -w /work bootroll-packaging:deb \
            bash scripts/packaging/deb/build-deb.sh smoke || failed=1
        chown_back bootroll-packaging:deb
        ;;
esac

case "$STAGES" in
    *rpm*|*all*)
        for fam in fedora opensuse; do
            img="bootroll-packaging:rpm-$fam"
            df="scripts/packaging/rpm/Dockerfile.$fam"
            build_image "$df" "$img" || { failed=1; continue; }
            run_stage "rpm-$fam" "$img" scripts/packaging/rpm/build-rpm.sh
            docker run --rm -v "$PWD:/work" -w /work "$img" \
                bash scripts/packaging/rpm/build-rpm.sh smoke "$fam" || failed=1
            chown_back "$img"
        done
        ;;
esac

case "$STAGES" in
    *arch*|*all*)
        build_image scripts/packaging/arch/Dockerfile bootroll-packaging:arch \
            && run_stage arch bootroll-packaging:arch scripts/packaging/arch/build-arch.sh
        docker run --rm -v "$PWD:/work" -w /work bootroll-packaging:arch \
            bash scripts/packaging/arch/build-arch.sh smoke || failed=1
        chown_back bootroll-packaging:arch
        ;;
esac

case "$STAGES" in
    *appimage*|*all*)
        build_image scripts/packaging/appimage/Dockerfile bootroll-packaging:appimage \
            && run_stage appimage bootroll-packaging:appimage scripts/packaging/appimage/build-appimage.sh
        docker run --rm -v "$PWD:/work" -w /work bootroll-packaging:appimage \
            bash scripts/packaging/appimage/build-appimage.sh smoke || failed=1
        chown_back bootroll-packaging:appimage
        ;;
esac

case "$STAGES" in
    *targz*|*all*)
        run_stage targz bootroll-packaging:appimage scripts/packaging/targz.sh
        ;;
esac

echo "=== sha256 ==="
(cd dist && sha256sum bootroll* > SHA256SUMS 2>/dev/null && cat SHA256SUMS) || true
chown "$(id -u):$(id -g)" dist/SHA256SUMS 2>/dev/null || true
echo "PACKAGE failed=$failed (0 = all stages green)"
exit $failed
