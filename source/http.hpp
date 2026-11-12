#pragma once
#include <string>

#include "common.hpp"

namespace http {

struct Result {
    long status = 0;
    std::string error;  // vacío si la transferencia en sí fue bien
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
    std::string describe() const;
};

// caPath: ruta a cacert.pem. Si no existe, TLS se usa SIN verificar certificados.
void init(const std::string& caPath);
void shutdown();
bool tlsVerified();

// GET a memoria (JSON, etc.)
Result get(const std::string& url, std::string& body, Progress* progress = nullptr, long timeoutSec = 30);
// Descarga a disco (escribe en dest + ".part" y renombra al terminar).
Result download(const std::string& url, const std::string& dest, Progress* progress);

}  // namespace http
