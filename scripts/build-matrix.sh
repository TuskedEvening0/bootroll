#!/bin/bash
# M9 distro build matrix (M9_PLAN §2): bake one container per distro family,
# then configure + build (0 errors / 0 warnings) + ctest inside each.
#
# Usage: scripts/build-matrix.sh [name ...]     (default: all)
# Requires docker; host runs the Arch/GCC+clang baseline itself.
set -u
cd "$(dirname "$0")/.." || exit 1

ALL="ubuntu2204 debian12 ubuntu2404 debian13 fedora tumbleweed"
names=${*:-$ALL}
failed=0

for n in $names; do
    echo "=== matrix: $n ==="
    docker build -q -f "scripts/matrix/Dockerfile.$n" -t "bootroll-matrix:$n" scripts/matrix || { echo "[$n] IMAGE BUILD FAILED"; failed=1; continue; }
    docker run --rm -v "$PWD:/work" -w /work "bootroll-matrix:$n" \
        bash scripts/matrix/run.sh "$n" gcc g++ || failed=1
done

# Optional clang leg (default floor config: ubuntu2204).
case " $* " in
    *" ubuntu2204 "*|true) ;;
esac
if [ $# -eq 0 ] || [ "$1" = ubuntu2204 ]; then
    echo "=== matrix: ubuntu2204-clang14 ==="
    docker run --rm -v "$PWD:/work" -w /work "bootroll-matrix:ubuntu2204" \
        bash scripts/matrix/run.sh ubuntu2204-clang14 clang clang++ || failed=1
fi

echo "=== summary ==="
echo "failed=$failed (0 = matrix green)"
exit $failed
