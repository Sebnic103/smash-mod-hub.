#include "audio.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "async.hpp"
#include "music.hpp"
#include "util.hpp"

namespace audio {

namespace {

constexpr size_t kMaxClipBytes = 32u * 1024u * 1024u;  // audio ya convertido; ~3 min en estéreo a 44,1 kHz

struct LoadRes {
    std::shared_ptr<wav::Clip> clip;
    std::string name, err;
    int idx = -1;
};

SDL_AudioDeviceID g_dev = 0;
bool g_paused = true;
music::Engine g_engine;
int g_mode = kOff;
int g_rate = 48000;

std::vector<std::string> g_files;  // nombres .wav de la carpeta
int g_idx = 0;                     // pista personalizada actual
std::string g_name, g_err;
Async<LoadRes>* g_job = nullptr;
bool g_wantLoad = false;
int g_failedInRow = 0;

// Hilo de audio de SDL: no debe lanzar excepciones ni tardar.
void callback(void*, Uint8* stream, int len) {
    if (len <= 0) return;
    try {
        g_engine.render((int16_t*)stream, len / 4);
    } catch (...) {
        memset(stream, 0, (size_t)len);
    }
}

void lockDev() {
    if (g_dev) SDL_LockAudioDevice(g_dev);
}
void unlockDev() {
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

bool isWav(const std::string& n) { return util::endsWith(util::lower(n), ".wav"); }

void startLoad() {
    if (!g_job || g_files.empty() || g_job->busy.load()) return;
    g_wantLoad = false;
    const int idx = g_idx % (int)g_files.size();
    const std::string path = util::join(customDir(), g_files[(size_t)idx]);
    const std::string name = g_files[(size_t)idx];
    g_job->run([path, name, idx]() {
        LoadRes r;
        r.idx = idx;
        r.name = name;
        auto clip = std::make_shared<wav::Clip>();
        if (wav::load(path, *clip, r.err, kMaxClipBytes)) r.clip = clip;
        return r;
    });
}

void applyMode() {
    lockDev();
    g_engine.setMode(g_mode == kChip ? music::Mode::Chip : g_mode == kCustom ? music::Mode::Custom : music::Mode::Off);
    unlockDev();
}

}  // namespace

std::string customDir() { return util::join(paths::data(), "music"); }

void rescan() {
    g_files.clear();
    for (const auto& f : util::listFiles(customDir()))
        if (isWav(f)) g_files.push_back(f);
    std::sort(g_files.begin(), g_files.end());
    if (g_idx >= (int)g_files.size()) g_idx = 0;
}

bool init(int mode, int volumePct) {
    if (g_dev) return true;
    try {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            util::log("audio: SDL_INIT_AUDIO falló: %s", SDL_GetError());
            return false;
        }
        SDL_AudioSpec want, have;
        SDL_zero(want);
        SDL_zero(have);
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = callback;
        g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // 0: SDL convierte si el dispositivo es distinto
        if (!g_dev) {
            util::log("audio: no se pudo abrir el dispositivo: %s", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        g_rate = have.freq > 0 ? have.freq : 48000;
        g_engine.init(g_rate);
        g_engine.setVolume(volumePct);
        g_job = new Async<LoadRes>();
        util::mkdirs(customDir());
        rescan();
        g_mode = (mode == kCustom && g_files.empty()) ? kChip : std::max(0, std::min(mode, 2));
        applyMode();
        g_paused = true;  // se activa en pump() cuando hay algo que sonar
        util::log("audio: %d Hz, %d archivo(s) .wav en %s", g_rate, (int)g_files.size(), customDir().c_str());
        return true;
    } catch (...) {
        util::log("audio: excepción al iniciar");
        return false;
    }
}

void shutdown() {
    if (g_job) {
        g_job->join();
    }
    if (g_dev) {
        SDL_PauseAudioDevice(g_dev, 1);
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    delete g_job;
    g_job = nullptr;
}

bool available() { return g_dev != 0; }

int setMode(int mode) {
    if (!g_dev) return kOff;
    mode = std::max(0, std::min(mode, 2));
    if (mode == kCustom) {
        rescan();
        if (g_files.empty()) mode = g_mode == kCustom ? kOff : g_mode;  // nada que reproducir: se queda como estaba
        else {
            g_err.clear();
            g_failedInRow = 0;
            g_wantLoad = true;
        }
    }
    g_mode = mode;
    if (g_mode != kCustom) {  // al salir de la música personalizada se libera la pista (puede ocupar decenas de MB)
        lockDev();
        g_engine.setClip(nullptr);
        unlockDev();
        g_name.clear();
    }
    applyMode();
    return g_mode;
}

int mode() { return g_mode; }

void setVolume(int pct) {
    if (!g_dev) return;
    lockDev();
    g_engine.setVolume(pct);
    unlockDev();
}

void next() {
    if (!g_dev) return;
    if (g_mode == kChip) {
        lockDev();
        g_engine.nextSong();
        unlockDev();
    } else if (g_mode == kCustom && !g_files.empty()) {
        g_idx = (g_idx + 1) % (int)g_files.size();
        g_wantLoad = true;
    }
}

void pump() {
    if (!g_dev) return;
    try {
        if (g_job) {
            LoadRes r;
            if (g_job->take(r)) {
                if (r.clip) {
                    lockDev();
                    g_engine.setClip(r.clip);
                    unlockDev();
                    g_name = r.name;
                    g_err.clear();
                    g_failedInRow = 0;
                } else {
                    util::log("audio: %s: %s", r.name.c_str(), r.err.c_str());
                    g_err = r.name + ": " + r.err;
                    if (++g_failedInRow < (int)g_files.size()) {  // prueba con la siguiente; si todas fallan se para
                        g_idx = (g_idx + 1) % (int)g_files.size();
                        g_wantLoad = true;
                    }
                }
            }
        }
        if (g_mode == kCustom) {
            bool finished = false;
            lockDev();
            finished = g_engine.hasClip() && g_engine.clipFinished();
            unlockDev();
            if (finished && !g_files.empty() && !(g_job && g_job->busy.load())) {
                g_idx = (g_idx + 1) % (int)g_files.size();
                g_wantLoad = true;
            }
            if (g_wantLoad) startLoad();
        }
        // El dispositivo se pausa cuando no suena nada (ahorra CPU), y se reanuda al volver a necesitarlo.
        lockDev();
        const bool idle = g_engine.idle();
        unlockDev();
        if (idle != g_paused) {
            SDL_PauseAudioDevice(g_dev, idle ? 1 : 0);
            g_paused = idle;
        }
    } catch (...) {
        sys::noteFault();
    }
}

float level() { return g_dev ? g_engine.level() : 0.f; }

std::string nowPlaying() {
    if (g_mode == kChip) return std::string(g_engine.songName());
    if (g_mode == kCustom) return g_name.empty() ? "..." : g_name;
    return "";
}

int customCount() { return (int)g_files.size(); }

std::string lastError() { return g_err; }

}  // namespace audio
