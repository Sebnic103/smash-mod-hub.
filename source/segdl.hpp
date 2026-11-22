#pragma once
// Descarga en trozos paralelos (varias conexiones con "Range"), escrita en orden en el archivo.
// No depende de curl: el transporte real vive en http.cpp y los tests usan uno falso.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "common.hpp"

namespace http {

struct Probe {
    bool ok = false;      // la petición de prueba respondió bien
    bool ranges = false;  // el servidor entiende "Range" (respondió 206)
    uint64_t size = 0;    // tamaño total del archivo (0 = desconocido)
    std::string url;      // URL final tras las redirecciones ("" = usar la original)
    long status = 0;
    std::string error;
};

enum class Fetch { Ok, Retry, Unsupported, Cancelled };
struct FetchResult {
    Fetch st = Fetch::Ok;
    std::string error;
};

class Transport {
public:
    virtual ~Transport() = default;
    // Pide los bytes 0-0 para saber si el servidor admite Range y cuánto pesa el archivo.
    virtual Probe probe(const std::string& url) = 0;
    // Pide los bytes [from, to] (inclusive) y los añade a out. Va sumando a "received" lo que llega.
    // Se puede llamar desde varios hilos a la vez.
    virtual FetchResult fetchRange(const std::string& url, uint64_t from, uint64_t to, std::string& out, const std::atomic<bool>& cancel, std::atomic<uint64_t>& received) = 0;
};

struct SegOptions {
    int conns = 4;                 // conexiones en paralelo
    uint64_t chunk = 1u << 20;     // tamaño de cada trozo
    int window = 12;               // trozos como máximo en memoria a la vez (adelantados al que toca escribir)
    int attempts = 4;              // intentos por trozo antes de rendirse
    // Reanudar: ya hay "start" bytes buenos al principio del archivo y el archivo está colocado justo ahí.
    uint64_t start = 0;
    // Se llama con cada trozo, EN ORDEN y justo antes de escribirlo (para calcular el MD5 sobre la marcha).
    std::function<void(const char*, size_t)> onData;
};

enum class SegStatus { Done, Cancelled, Unsupported, Failed };
struct SegResult {
    SegStatus st = SegStatus::Failed;
    std::string error;
    bool diskError = false;  // el fallo fue al escribir en la SD (no tiene sentido reintentar con otra conexión)
    uint64_t written = 0;    // bytes buenos escritos desde el principio del archivo (incluye "start"); sirve para reanudar
};

// Descarga los bytes [opt.start, probe.size) de url y los escribe EN ORDEN en out (posición opt.start; vacío si start=0).
// Unsupported = el servidor no sirve trozos o no se pudieron crear hilos: el llamador debe volver a empezar
// con una descarga normal (truncando el archivo).
SegResult segmentedDownload(Transport& t, const std::string& url, const Probe& probe, FILE* out, Progress* prog, const SegOptions& opt = SegOptions());

}  // namespace http
