#pragma once
#include <SDL2/SDL.h>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common.hpp"
#include "sys.hpp"

// Descarga (si es una URL) y decodifica imágenes en hilos de fondo; las texturas se crean
// en el hilo principal con pump(). Las URLs se guardan en disco para no volver a bajarlas.
class ImageCache {
public:
    ~ImageCache() { shutdown(); }

    void init(SDL_Renderer* renderer, const std::string& diskDir, int workers = 3);
    void shutdown();

    // Devuelve la textura si ya está lista; si no, la encola y devuelve nullptr. Mientras se siga pidiendo cada
    // frame, la petición se mantiene viva; las que dejan de pedirse (p. ej. al cambiar de página) se descartan
    // sin llegar a descargarse, y los hilos atienden primero lo que se está viendo ahora.
    SDL_Texture* get(const std::string& src, bool isUrl);
    // Llamar una vez por frame desde el hilo principal.
    void pump(int maxUploadsPerFrame = 3);
    // Olvida una imagen (p. ej. si el archivo local cambió).
    void invalidate(const std::string& src);
    void clearDisk();

private:
    enum class St { Queued, Loading, Ready, Done, Failed };
    struct Entry {
        St st = St::Queued;
        bool isUrl = false;
        SDL_Surface* surf = nullptr;
        SDL_Texture* tex = nullptr;
        uint64_t lastUse = 0;
        uint64_t failedAt = 0;
    };

    static constexpr uint64_t kStaleFrames = 120;  // ~2 s a 60 fps
    void workerLoop();
    SDL_Surface* load(const std::string& src, bool isUrl);

    SDL_Renderer* r_ = nullptr;
    std::string dir_;
    std::map<std::string, Entry> map_;
    std::vector<std::string> queue_;
    sys::Mutex mx_;
    std::vector<std::unique_ptr<sys::Thread>> workers_;
    std::atomic<bool> quit_{false};
    Progress cancel_;  // para cortar las descargas en curso al cerrar
    size_t nReady_ = 0;  // entradas con superficie lista para subir a textura
    uint64_t frame_ = 1;
    size_t texCount_ = 0;
};
