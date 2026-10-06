#!/bin/bash
# Matrix runner - executes INSIDE a container with /work = repo root.
# Usage: run.sh <name> <cc> <cxx>
# Prints one summary line on success; diagnostics + non-zero exit on failure.
set -u
name=${1:?name}; cc=${2:?cc}; cxx=${3:?cxx}
bdir="build/linux-matrix-$name"

echo "== [$name] toolchain: $($cxx --version | head -1) | $(cmake --version | head -1)"

cmake -S . -B "$bdir" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
      >"/tmp/conf_$name.log" 2>&1
if [ $? -ne 0 ]; then
    echo "[$name] CONFIGURE FAILED"; tail -25 "/tmp/conf_$name.log"; exit 1
fi

cmake --build "$bdir" -j"$(nproc)" >"/tmp/build_$name.log" 2>&1
rc=$?
w=$(grep -ci "warning" "/tmp/build_$name.log" || true)
e=$(grep -ci "error" "/tmp/build_$name.log" || true)
if [ $rc -ne 0 ] || [ "$w" -ne 0 ] || [ "$e" -ne 0 ]; then
    echo "[$name] BUILD FAILED rc=$rc warnings=$w errors=$e"
    grep -iE -B2 -A6 "warning|error" "/tmp/build_$name.log" | head -60
    exit 1
fi

ctest --test-dir "$bdir" -V >"/tmp/ctest_$name.log" 2>&1
if ! grep -q "Status: SUCCESS" "/tmp/ctest_$name.log"; then
    echo "[$name] CTEST FAILED"; grep -E "FAILED|assertions:" "/tmp/ctest_$name.log" | head; exit 1
fi
cases=$(grep -E "test cases:" "/tmp/ctest_$name.log" | head -1)
asserts=$(grep -E "assertions:" "/tmp/ctest_$name.log" | head -1)
echo "[$name] OK  0 err 0 warn | $cases | $asserts"
