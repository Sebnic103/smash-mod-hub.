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

// Teclado del sistema. false si el usuario cancela o no está disponible.
bool textInput(const std::string& header, const std::string& initial, std::string& out, int maxLen = 60, bool numeric = false);

}  // namespace platform
