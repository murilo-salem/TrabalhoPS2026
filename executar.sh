#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")"
porta="${1:-8080}"

recompilar=0
[ -x build/sicxe_sim ] || recompilar=1
for fonte in src/*.cpp src/*.hpp tests/*.cpp tests/*.py CMakeLists.txt compilar.sh; do
    if [ "$fonte" -nt build/sicxe_sim ]; then recompilar=1; fi
done
if [ "$recompilar" = 1 ]; then sh ./compilar.sh; fi

url="http://127.0.0.1:$porta"
echo "Abrindo $url"
if [ "${SICXE_NO_BROWSER:-0}" != 1 ] && command -v xdg-open >/dev/null 2>&1; then
    (sleep 1; xdg-open "$url" >/dev/null 2>&1) &
fi
exec ./build/sicxe_sim "$porta"
