#include "music.hpp"

#include <algorithm>
#include <cmath>

namespace music {

float masterGain(int pct) {
    pct = std::max(0, std::min(100, pct));
    return 0.8f * std::pow((float)pct / 100.f, 1.5f);
}

void Engine::init(int sampleRate) {
    rate_ = std::max(8000, std::min(sampleRate, 192000));
    chip_.init(rate_);
    chipGain_ = wavGain_ = 0.f;
    volGain_ = volTarget_;
}

void Engine::setVolume(int pct) { volTarget_ = masterGain(pct); }

// Compresión suave en vez de recortar: x - 4/27 x^3 llega a 1.0 en x = 1.5 con pendiente 0.
static inline float softClip(float x) {
    if (x > 1.5f) x = 1.5f;
    if (x < -1.5f) x = -1.5f;
    return x - 0.148148f * x * x * x;
}

void Engine::render(int16_t* out, int frames) {
    if (frames <= 0) return;
    const size_t n = (size_t)frames * 2;
    if (idle()) {
        std::fill(out, out + n, (int16_t)0);
        level_.store(0.f, std::memory_order_relaxed);
        return;
    }
    if (a_.size() < n) {
        a_.resize(n);
        b_.resize(n);
    }
    const float chipTarget = mode_ == Mode::Chip ? 1.f : 0.f;
    const float wavTarget = mode_ == Mode::Custom ? 1.f : 0.f;
    const bool needChip = chipTarget > 0.f || chipGain_ > 0.f;
    const bool needWav = wavTarget > 0.f || wavGain_ > 0.f;
    if (needChip) chip_.render(a_.data(), frames);
    if (needWav) player_.render(b_.data(), frames, rate_);
    const float fadeStep = 1.f / (0.25f * (float)rate_);  // 250 ms
    const float volStep = 1.f / (0.2f * (float)rate_);  // los cambios de volumen tardan ~0.2 s: sin chasquidos
    double acc = 0.0;
    for (int i = 0; i < frames; i++) {
        chipGain_ += std::max(-fadeStep, std::min(fadeStep, chipTarget - chipGain_));
        wavGain_ += std::max(-fadeStep, std::min(fadeStep, wavTarget - wavGain_));
        volGain_ += std::max(-volStep, std::min(volStep, volTarget_ - volGain_));
        float l = 0.f, r = 0.f;
        if (needChip) {
            l += a_[(size_t)i * 2] * chipGain_;
            r += a_[(size_t)i * 2 + 1] * chipGain_;
        }
        if (needWav) {
            l += b_[(size_t)i * 2] * wavGain_;
            r += b_[(size_t)i * 2 + 1] * wavGain_;
        }
        l = softClip(l * volGain_);
        r = softClip(r * volGain_);
        acc += (double)(l * l + r * r) * 0.5;
        out[(size_t)i * 2] = (int16_t)std::lrintf(std::max(-1.f, std::min(1.f, l)) * 32767.f);
        out[(size_t)i * 2 + 1] = (int16_t)std::lrintf(std::max(-1.f, std::min(1.f, r)) * 32767.f);
    }
    // nivel para el ecualizador: RMS escalado (el volumen ya está aplicado, así que se normaliza por el máximo teórico)
    const float rms = (float)std::sqrt(acc / frames);
    const float ref = std::max(0.05f, volTarget_ * 0.5f);
    const float v = std::min(1.f, rms / ref);
    const float old = level_.load(std::memory_order_relaxed);
    level_.store(old + (v - old) * 0.5f, std::memory_order_relaxed);
}

}  // namespace music
