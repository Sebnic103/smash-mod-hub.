#pragma once
// Motor de música (sin SDL): mezcla el sintetizador chiptune o la pista .wav del usuario, aplica volumen con
// fundidos (sin chasquidos al activar/desactivar) y entrega audio estéreo de 16 bits. audio.cpp lo conecta a SDL.
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "synth.hpp"
#include "wavfile.hpp"

namespace music {

enum class Mode { Off = 0, Chip = 1, Custom = 2 };

class Engine {
public:
    void init(int sampleRate);
    void setMode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }
    void setVolume(int pct);
    void nextSong() { chip_.next(); }
    const char* songName() const { return chip_.name(); }
    void setClip(std::shared_ptr<const wav::Clip> c) { player_.set(std::move(c)); }
    bool clipFinished() const { return player_.finished(); }
    bool hasClip() const { return player_.hasClip(); }
    // Fotogramas estéreo S16 intercalados. Siempre escribe `frames` fotogramas.
    void render(int16_t* out, int frames);
    // Nivel de salida reciente (0..1) para el ecualizador de la interfaz. Se puede leer desde otro hilo.
    float level() const { return level_.load(std::memory_order_relaxed); }
    // ¿suena algo ahora (o se está apagando)? Si no, el dispositivo de audio puede pausarse.
    bool idle() const { return mode_ == Mode::Off && chipGain_ <= 0.f && wavGain_ <= 0.f; }

private:
    int rate_ = 48000;
    Mode mode_ = Mode::Off;
    float volGain_ = 0.f, volTarget_ = 0.37f, chipGain_ = 0.f, wavGain_ = 0.f;
    synth::ChipSynth chip_;
    wav::Player player_;
    std::vector<float> a_, b_;
    std::atomic<float> level_{0.f};
};

float masterGain(int pct);  // curva de volumen (0..100 -> 0..~0.8)

}  // namespace music
