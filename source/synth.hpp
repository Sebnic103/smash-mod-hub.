#pragma once
// Sintetizador chiptune con melodías ORIGINALES (nada de música con copyright): ondas de pulso, triángulo y ruido
// como las consolas de 8 bits. Las canciones son texto (acordes + melodía) y se convierten en una tabla de
// "pasos" (semicorcheas); el render solo suma unas pocas ondas por muestra, así que casi no gasta CPU.
// Núcleo sin SDL ni libnx: se prueba en el PC (tests/test_audio.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace synth {

struct SongDef {
    const char* name;
    int bpm;
    const char* chords;  // un acorde por compás ("C G Am F"): triadas mayores/menores, con # o b
    const char* lead;    // melodía: "E5:2 G5:2 r:4 ..." (nota:duración en pasos de 1/16; r = silencio); 16 pasos por compás
    float duty;          // ancho del pulso de la melodía (0.5 = cuadrada); 0 = onda triangular
    int bassStyle;       // 0 = corcheas con salto de octava, 1 = tranquilo (redonda + quinta)
    int arpStyle;        // 0 = arpegio de semicorcheas, 1 = arpegio de corcheas suave
    bool drums;
};

constexpr int kSongs = 3;
const SongDef& songDef(int i);

// Un paso = una semicorchea. Las duraciones se miden en pasos; 0 Hz = nada nuevo en ese paso.
struct Step {
    float leadHz = 0.f;
    int leadLen = 0;
    float bassHz = 0.f;
    int bassLen = 0;
    float arpHz = 0.f;
    uint8_t drums = 0;  // 1 bombo, 2 caja, 4 charles
};

enum : uint8_t { kKick = 1, kSnare = 2, kHat = 4 };

// "C#5" -> frecuencia en Hz (0 si no es una nota). *end apunta al primer carácter sin leer.
double noteHz(const char* s, const char** end = nullptr);
// "Am", "F#", "Bb" -> semitono de la raíz (0 = do) y si es menor. false si no es un acorde.
bool parseChord(const std::string& s, int& rootSemi, bool& minor);
// Convierte la canción en pasos. false (y err) si la notación está mal o un compás no suma 16 pasos.
bool buildSteps(const SongDef& d, std::vector<Step>& out, std::string* err = nullptr);

class ChipSynth {
public:
    void init(int sampleRate);
    void setSong(int idx);  // empieza desde el principio, sin fundido
    void next();            // pasa a la siguiente canción con un fundido corto
    int song() const { return song_; }
    const char* name() const;
    // Escribe `frames` fotogramas estéreo intercalados (L,R) en out (sobrescribe). Amplitud máxima ~0.8.
    void render(float* out, int frames);

private:
    struct Voice {
        double phase = 0.0, inc = 0.0;
        float env = 0.f;
        int held = 0;        // muestras que le quedan con la nota sostenida
        int age = 0;         // muestras desde que empezó
        bool on = false;
    };
    struct Drum {
        int age = -1, kind = 0;
        float phase = 0.f, lp = 0.f;
    };
    void startStep();
    void trigger(Voice& v, float hz, int lenSamples);
    float voiceSample(Voice& v, int wave, double duty, float sustain, float decayTau);
    float noise();

    int rate_ = 48000, song_ = 0, step_ = 0, loops_ = 0;
    int samplesPerStep_ = 6000, posInStep_ = 0;
    float fade_ = 1.f, fadeTarget_ = 1.f;
    bool switching_ = false;
    std::vector<Step> steps_;
    Voice lead_, bass_, arp_;
    Drum drum_[2];
    uint32_t rng_ = 0x1234567u;
    float lp_[2] = {0.f, 0.f};
    int vibAge_ = 0;
};

}  // namespace synth
