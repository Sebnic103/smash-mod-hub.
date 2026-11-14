#!/bin/sh
# Compila y ejecuta los tests del núcleo en tu PC (no necesita devkitPro).
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/smash_mod_hub_tests"
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
  test_core.cpp stub_http.cpp \
  ../source/sys.cpp ../source/util.cpp ../source/json.cpp ../source/md5.cpp \
  ../source/segdl.cpp ../source/settings.cpp ../source/modstore.cpp ../source/installer.cpp ../source/gamebanana.cpp \
  -o "$OUT" -lpthread
"$OUT"
# Lógica de texto de la interfaz (ajuste de línea, caché por frame) con un SDL/TTF falso
OUT_UI="${TMPDIR:-/tmp}/smash_mod_hub_test_ui"
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined -Istubs \
  test_ui.cpp ../source/ui.cpp -o "$OUT_UI"
"$OUT_UI"
