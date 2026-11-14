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
// Cierra las conexiones que llevan >30 s sin usarse (llamar de vez en cuando desde un hilo de fondo).
void trimIdle();
// Al salir de la app: corta de inmediato todas las transferencias en curso (y las nuevas fallan rápido).
void abortAll();

// GET a memoria (JSON, etc.)
Result get(const std::string& url, std::string& body, Progress* progress = nullptr, long timeoutSec = 30);

// Descarga pequeña a disco (miniaturas, imágenes): una sola conexión, reutilizada entre peticiones y sin
// escribir nada en el registro si sale bien. Escribe en dest + ".part" y renombra al terminar.
Result download(const std::string& url, const std::string& dest, Progress* progress = nullptr);

// Descarga de un archivo grande (mod): hasta "conns" conexiones en paralelo si el servidor admite Range;
// si no, o si algo falla, una sola conexión. Actualiza progress->now/total/bps.
Result downloadBig(const std::string& url, const std::string& dest, Progress* progress, int conns);

}  // namespace http
