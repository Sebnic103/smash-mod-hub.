#!/bin/sh
# Compila y ejecuta los tests del núcleo en tu PC (no necesita devkitPro).
# Usa detectores de errores de memoria (ASan/UBSan); si el entorno no los soporta (algunos servidores de CI
# hacen que el ejecutable se cierre nada más arrancar) repite la prueba sin ellos.
cd "$(dirname "$0")" || exit 1
TMP="${TMPDIR:-/tmp}"

suite() {  # $1 = ejecutable, resto = fuentes y opciones
    out=$1
    shift
    g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined "$@" -o "$out" || return 1
    "$out"
    rc=$?
    if [ "$rc" -ge 128 ]; then
        echo "Aviso: el ejecutable con sanitizers terminó con código $rc (¿entorno sin soporte?). Se repite sin ellos."
        g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -g "$@" -o "$out" || return 1
        "$out"
        rc=$?
    fi
    return $rc
}

suite "$TMP/smash_mod_hub_tests" \
    test_core.cpp stub_http.cpp \
    ../source/sys.cpp ../source/util.cpp ../source/json.cpp ../source/md5.cpp \
    ../source/segdl.cpp ../source/settings.cpp ../source/modstore.cpp ../source/installer.cpp ../source/gamebanana.cpp ../source/reslot.cpp \
    -lpthread || exit $?

# Lógica de texto de la interfaz (ajuste de línea, caché por frame) con un SDL/TTF falso
suite "$TMP/smash_mod_hub_test_ui" -Istubs test_ui.cpp ../source/ui.cpp || exit $?

# Teclado en pantalla (lógica de texto, cursor, navegación y toques)
suite "$TMP/smash_mod_hub_test_osk" test_osk.cpp ../source/osk.cpp || exit $?

# Música: notas, canciones, sintetizador, lectura de .wav y motor de mezcla
suite "$TMP/smash_mod_hub_test_audio" test_audio.cpp ../source/synth.cpp ../source/wavfile.cpp ../source/music.cpp || exit $?
