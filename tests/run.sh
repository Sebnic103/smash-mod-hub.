#!/bin/sh
# Compila y ejecuta los tests del núcleo en tu PC (no necesita devkitPro).
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/smash_mod_hub_tests"
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
  test_core.cpp stub_http.cpp \
  ../source/sys.cpp ../source/util.cpp ../source/json.cpp ../source/md5.cpp \
  ../source/settings.cpp ../source/modstore.cpp ../source/installer.cpp ../source/gamebanana.cpp \
  -o "$OUT" -lpthread
"$OUT"
