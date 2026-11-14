#pragma once
#include <atomic>
#include <cstdint>

// Progreso compartido entre hilos (descargas / extracción).
struct Progress {
    std::atomic<uint64_t> now{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> bps{0};  // velocidad de descarga suavizada, bytes/s (0 = desconocida)

    double fraction() const {
        uint64_t t = total.load();
        return t ? (double)now.load() / (double)t : 0.0;
    }
};

// Velocidad suavizada: se alimenta con (tiempo, bytes acumulados) y devuelve bytes/s.
// Mide en ventanas de ~400 ms y las mezcla con una media exponencial para que el número no salte.
class SpeedMeter {
public:
    uint64_t update(uint64_t tMs, uint64_t bytes) {
        if (!started_) {
            started_ = true;
            lastT_ = tMs;
            lastB_ = bytes;
            return 0;
        }
        if (tMs < lastT_ + 400) return (uint64_t)bps_;
        double inst = (bytes >= lastB_ ? (double)(bytes - lastB_) : 0.0) * 1000.0 / (double)(tMs - lastT_);
        bps_ = bps_ <= 0.0 ? inst : bps_ * 0.7 + inst * 0.3;
        lastT_ = tMs;
        lastB_ = bytes;
        return (uint64_t)bps_;
    }

private:
    bool started_ = false;
    uint64_t lastT_ = 0, lastB_ = 0;
    double bps_ = 0.0;
};
