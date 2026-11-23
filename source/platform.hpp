#pragma once
#include <string>

#include "input.hpp"

namespace platform {

class Pad {
public:
    Pad();
    ~Pad();
    Pad(const Pad&) = delete;
    Pad& operator=(const Pad&) = delete;

    void init();
    Input poll();

private:
    struct Impl;
    Impl* impl_;
};

// Versión del firmware de la consola ("23.0.0"), o "" si no se puede leer.
std::string firmwareVersion();

// Sube (true) o baja (false) la velocidad de la CPU de la consola. Se usa solo mientras hay descargas/instalaciones
// (descomprimir 7z/RAR es trabajo de CPU). Sin efecto si el sistema no lo permite.
void cpuBoost(bool on);

// true si la app corre como applet (abierta desde el Álbum): tiene menos memoria y no puede abrir otras applets
// con seguridad. Conviene abrir los homebrew con un juego (mantener R al lanzar un juego).
bool appletMode();

// Teclado del sistema. false si el usuario cancela o no está disponible.
bool textInput(const std::string& header, const std::string& initial, std::string& out, int maxLen = 60, bool numeric = false);

}  // namespace platform
