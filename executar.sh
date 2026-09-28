#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")"
porta="${1:-8080}"

if [ ! -x build/sicxe_sim ]; then
    if command -v cmake >/dev/null 2>&1 && command -v c++ >/dev/null 2>&1; then
        cmake -S . -B build
        cmake --build build -j2
    elif command -v podman >/dev/null 2>&1; then
        podman run --rm -v "$PWD":/work:Z -w /work docker.io/library/alpine:3.21 \
            sh -c 'apk add --no-cache g++ cmake make >/dev/null && cmake -S . -B build -DCMAKE_EXE_LINKER_FLAGS=-static && cmake --build build -j2'
    else
        echo 'Instale CMake e um compilador C++17 para construir o simulador.' >&2
        exit 1
    fi
fi

url="http://127.0.0.1:$porta"
echo "Abrindo $url"
if [ "${SICXE_NO_BROWSER:-0}" != 1 ] && command -v xdg-open >/dev/null 2>&1; then
    (sleep 1; xdg-open "$url" >/dev/null 2>&1) &
fi
exec ./build/sicxe_sim "$porta"
