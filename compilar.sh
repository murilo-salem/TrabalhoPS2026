#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"

if command -v cmake >/dev/null 2>&1 && command -v c++ >/dev/null 2>&1; then
    cmake -S . -B build/local
    cmake --build build/local -j2
    ctest --test-dir build/local --output-on-failure
    cp build/local/sicxe_sim build/sicxe_sim
elif command -v podman >/dev/null 2>&1; then
    podman run --rm -v "$PWD":/work:Z -w /work docker.io/library/alpine:3.21 \
        sh -c 'apk add --no-cache g++ cmake make python3 >/dev/null && cmake -S . -B build/container -DCMAKE_EXE_LINKER_FLAGS=-static && cmake --build build/container -j2 && ctest --test-dir build/container --output-on-failure && cp build/container/sicxe_sim build/sicxe_sim'
else
    echo 'Instale CMake e um compilador C++17 (ou Podman) para compilar e testar.' >&2
    exit 1
fi
