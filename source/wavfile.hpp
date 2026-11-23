#pragma once
// Lector de .wav para la música personalizada (sd:/switch/smash-mod-hub/music/*.wav) y reproductor con
// remuestreo lineal. Núcleo sin SDL: se prueba en el PC. Lee el archivo por bloques (no lo carga entero a
// memoria) y limita el tamaño para no quedarse sin RAM, sobre todo si la app corre como applet.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wav {

struct Clip {
    int rate = 0;
    int channels = 2;       // 1 o 2
    std::vector<int16_t> pcm;  // intercalado si es estéreo
    size_t frames() const { return channels > 0 ? pcm.size() / (size_t)channels : 0; }
};

// PCM entero de 8/16/24/32 bits o float de 32 bits, mono o estéreo (también "extensible"). No admite ADPCM/MP3 dentro
// del WAV. maxBytes limita lo que ocupará el audio ya convertido (16 bits). false y err si no se puede.
bool load(const std::string& path, Clip& out, std::string& err, size_t maxBytes);

// Reproduce un Clip una vez, con remuestreo lineal a la frecuencia de salida.
class Player {
public:
    void set(std::shared_ptr<const Clip> c) {
        clip_ = std::move(c);
        pos_ = 0.0;
    }
    bool hasClip() const { return clip_ != nullptr; }
    bool finished() const { return clip_ && pos_ >= (double)clip_->frames() - 1.0; }
    // Sobrescribe `frames` fotogramas estéreo float (L,R). Sin pista o al acabar: silencio.
    void render(float* out, int frames, int dstRate);

private:
    std::shared_ptr<const Clip> clip_;
    double pos_ = 0.0;
};

}  // namespace wav
