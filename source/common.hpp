#pragma once
#include <atomic>
#include <cstdint>

// Progreso compartido entre hilos (descargas / extracción).
struct Progress {
    std::atomic<uint64_t> now{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};

    double fraction() const {
        uint64_t t = total.load();
        return t ? (double)now.load() / (double)t : 0.0;
    }
};
