#include "imagecache.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>

#include "http.hpp"
#include "util.hpp"

void ImageCache::init(SDL_Renderer* renderer, const std::string& diskDir, int workers) {
    r_ = renderer;
    dir_ = diskDir;
    util::mkdirs(dir_);
    quit_ = false;
    cancel_.cancel = false;
    for (int i = 0; i < workers; i++) {
        auto t = std::make_unique<sys::Thread>();
        t->start([this] { workerLoop(); }, 0x100000);
        workers_.push_back(std::move(t));
    }
}

void ImageCache::shutdown() {
    quit_ = true;
    cancel_.cancel = true;
    for (auto& t : workers_) t->join();
    workers_.clear();
    sys::Lock l(mx_);
    for (auto& kv : map_) {
        if (kv.second.surf) SDL_FreeSurface(kv.second.surf);
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    }
    map_.clear();
    queue_.clear();
    texCount_ = 0;
    nReady_ = 0;
}

SDL_Texture* ImageCache::get(const std::string& src, bool isUrl) {
    if (src.empty()) return nullptr;
    sys::Lock l(mx_);
    auto it = map_.find(src);
    if (it == map_.end()) {
        Entry e;
        e.isUrl = isUrl;
        e.lastUse = frame_;
        map_[src] = e;
        queue_.push_back(src);
        return nullptr;
    }
    Entry& e = it->second;
    e.lastUse = frame_;
    if (e.st == St::Done) return e.tex;
    if (e.st == St::Failed && sys::nowMs() - e.failedAt > 30000) {  // reintento cada 30 s
        e.st = St::Queued;
        queue_.push_back(src);
    }
    return nullptr;
}

void ImageCache::invalidate(const std::string& src) {
    sys::Lock l(mx_);
    auto it = map_.find(src);
    if (it == map_.end()) return;
    if (it->second.st == St::Loading) return;  // el worker la está usando; se refrescará en la próxima carga
    if (it->second.tex) {
        SDL_DestroyTexture(it->second.tex);
        if (texCount_ > 0) texCount_--;
    }
    if (it->second.surf) SDL_FreeSurface(it->second.surf);
    if (it->second.st == St::Ready && nReady_ > 0) nReady_--;
    map_.erase(it);
}

void ImageCache::clearDisk() {
    util::rmrf(dir_);
    util::mkdirs(dir_);
}

void ImageCache::pump(int maxUploads) {
    sys::Lock l(mx_);
    frame_++;
    if (nReady_ > 0) {
        int budget = maxUploads * 4;  // una imagen grande gasta más que una miniatura
        for (auto& kv : map_) {
            Entry& e = kv.second;
            if (e.st != St::Ready) continue;
            if (budget <= 0) break;
            budget -= (e.surf && (int64_t)e.surf->w * e.surf->h > 300000) ? 4 : 1;
            e.tex = SDL_CreateTextureFromSurface(r_, e.surf);
            SDL_FreeSurface(e.surf);
            e.surf = nullptr;
            e.lastUse = frame_;
            nReady_--;
            if (e.tex) {
                e.st = St::Done;
                texCount_++;
            } else {
                e.st = St::Failed;
                e.failedAt = sys::nowMs();
            }
        }
    }
    // Expulsa las texturas menos usadas si hay demasiadas (se vuelven a cargar desde el disco).
    if (texCount_ > 150) {
        std::vector<std::pair<uint64_t, std::string>> cand;
        for (auto& kv : map_)
            if (kv.second.st == St::Done && kv.second.lastUse + 2 < frame_) cand.emplace_back(kv.second.lastUse, kv.first);
        std::sort(cand.begin(), cand.end());
        for (auto& c : cand) {
            if (texCount_ <= 110) break;
            auto it = map_.find(c.second);
            SDL_DestroyTexture(it->second.tex);
            map_.erase(it);
            texCount_--;
        }
    }
}

SDL_Surface* ImageCache::load(const std::string& src, bool isUrl) {
    std::string path = src;
    if (isUrl) {
        path = dir_ + "/" + util::fnvHex(src) + ".img";
        if (!util::isFile(path) || util::fileSize(path) == 0) {
            http::Result r = http::download(src, path, &cancel_);
            if (!r.ok()) return nullptr;
        }
    }
    SDL_Surface* s = IMG_Load(path.c_str());
    if (!s) {
        if (isUrl) util::rmrf(path);  // descarga corrupta: se vuelve a bajar en el reintento
        return nullptr;
    }
    SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(s);
    if (!c) return nullptr;
    const int maxW = 800, maxH = 600;  // ahorra memoria de vídeo
    if (c->w > maxW || c->h > maxH) {
        double k = std::min(maxW / (double)c->w, maxH / (double)c->h);
        int nw = std::max(1, (int)(c->w * k)), nh = std::max(1, (int)(c->h * k));
        SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, nw, nh, 32, SDL_PIXELFORMAT_RGBA32);
        if (d) {
            SDL_SetSurfaceBlendMode(c, SDL_BLENDMODE_NONE);
            SDL_BlitScaled(c, nullptr, d, nullptr);
            SDL_FreeSurface(c);
            return d;
        }
    }
    return c;
}

void ImageCache::workerLoop() {
    while (!quit_.load()) {
        std::string key;
        bool isUrl = false;
        {
            sys::Lock l(mx_);
            // Entre las pedidas, primero la más reciente (lo que se ve ahora); a igualdad, por orden de llegada.
            // Las que llevan >2 s sin pedirse (ya no se ven) se tiran sin descargar.
            size_t bestIdx = 0;
            uint64_t bestUse = 0;
            bool found = false;
            for (size_t i = 0; i < queue_.size();) {
                auto it = map_.find(queue_[i]);
                if (it == map_.end() || it->second.st != St::Queued) {
                    queue_.erase(queue_.begin() + (long)i);
                    continue;
                }
                if (it->second.lastUse + kStaleFrames < frame_) {
                    map_.erase(it);
                    queue_.erase(queue_.begin() + (long)i);
                    continue;
                }
                if (!found || it->second.lastUse > bestUse) {
                    found = true;
                    bestUse = it->second.lastUse;
                    bestIdx = i;
                }
                i++;
            }
            if (found) {
                key = queue_[bestIdx];
                queue_.erase(queue_.begin() + (long)bestIdx);
                Entry& e = map_[key];
                e.st = St::Loading;
                isUrl = e.isUrl;
            }
        }
        if (key.empty()) {
            sys::sleepMs(25);
            continue;
        }
        SDL_Surface* s = load(key, isUrl);
        sys::Lock l(mx_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            if (s) SDL_FreeSurface(s);
            continue;
        }
        if (s) {
            it->second.surf = s;
            it->second.st = St::Ready;
            nReady_++;
        } else {
            it->second.st = St::Failed;
            it->second.failedAt = sys::nowMs();
        }
    }
}
