#include "theme.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "util.hpp"

namespace theme {

namespace {

constexpr int W = 1280, H = 720;
constexpr int BW = 640, BH = 360;  // el fondo se genera a media resolución y el renderizador lo estira (suave y 4 veces más barato)
SDL_Renderer* R = nullptr;
SDL_Texture* g_bg = nullptr;
SDL_Texture* g_streak = nullptr;
bool g_custom = false;

inline Uint8 clamp8(float v) { return (Uint8)(v < 0.f ? 0.f : v > 255.f ? 255.f : v); }

SDL_Surface* newSurface(int w, int h) { return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32); }

// Fondo por defecto: degradado vertical oscuro, un resplandor de color arriba a la derecha, otro azulado abajo a
// la izquierda, franjas diagonales casi invisibles y viñeta en los bordes.
SDL_Texture* proceduralBg() {
    SDL_Surface* s = newSurface(BW, BH);
    if (!s || !s->pixels) {
        if (s) SDL_FreeSurface(s);
        return nullptr;
    }
    for (int y = 0; y < BH; y++) {
        Uint8* row = (Uint8*)s->pixels + (size_t)y * (size_t)s->pitch;
        const float ty = (float)y / (BH - 1);
        const float br = 25.f + (11.f - 25.f) * ty, bg = 27.f + (12.f - 27.f) * ty, bb = 43.f + (19.f - 43.f) * ty;
        for (int x = 0; x < BW; x++) {
            const float dx = (x - 550.f) / 350.f, dy = (y - 30.f) / 350.f;
            float g1 = std::max(0.f, 1.f - (dx * dx + dy * dy));
            g1 *= g1;
            const float ex = (x - 60.f) / 300.f, ey = (y - 360.f) / 300.f;
            float g2 = std::max(0.f, 1.f - (ex * ex + ey * ey));
            g2 *= g2;
            const float band = (((x + (int)(y * 0.7f)) % 32) < 5) ? 4.f : 0.f;
            const float vx = (x - BW / 2) / (float)(BW / 2), vy = (y - BH / 2) / (float)(BH / 2);
            const float vig = 1.f - 0.20f * (vx * vx * 0.6f + vy * vy * 0.4f);
            row[x * 4 + 0] = clamp8((br + 233.f * 0.17f * g1 + 40.f * 0.08f * g2 + band) * vig);
            row[x * 4 + 1] = clamp8((bg + 69.f * 0.17f * g1 + 120.f * 0.08f * g2 + band) * vig);
            row[x * 4 + 2] = clamp8((bb + 96.f * 0.17f * g1 + 255.f * 0.08f * g2 + band) * vig);
            row[x * 4 + 3] = 255;
        }
    }
    SDL_Texture* t = SDL_CreateTextureFromSurface(R, s);
    SDL_FreeSurface(s);
    return t;
}

// Imagen del usuario: se recorta al formato 16:9, se oscurece (para que el texto se lea) y se le pone viñeta.
SDL_Texture* customBg(const std::string& path) {
    if (path.empty() || !util::isFile(path)) return nullptr;
    if (util::fileSize(path) > 12u * 1024u * 1024u) {
        util::log("fondo: %s es demasiado grande (maximo 12 MB)", path.c_str());
        return nullptr;
    }
    SDL_Surface* raw = IMG_Load(path.c_str());
    if (!raw) return nullptr;
    SDL_Surface* src = nullptr;
    SDL_Surface* dst = nullptr;
    SDL_Texture* tex = nullptr;
    if (raw->w > 0 && raw->h > 0 && raw->w <= 8192 && raw->h <= 8192) src = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(raw);
    if (src) dst = newSurface(BW, BH);
    if (dst && dst->pixels) {
        SDL_Rect r = {0, 0, src->w, src->h};
        const double sa = (double)src->w / src->h, da = (double)BW / BH;
        if (sa > da) {
            r.w = (int)(src->h * da);
            r.x = (src->w - r.w) / 2;
        } else {
            r.h = (int)(src->w / da);
            r.y = (src->h - r.h) / 2;
        }
        SDL_BlitScaled(src, &r, dst, nullptr);
        for (int y = 0; y < BH; y++) {
            Uint8* row = (Uint8*)dst->pixels + (size_t)y * (size_t)dst->pitch;
            const float vy = (y - BH / 2) / (float)(BH / 2);
            for (int x = 0; x < BW; x++) {
                const float vx = (x - BW / 2) / (float)(BW / 2);
                const float k = 0.36f * (1.f - 0.25f * (vx * vx * 0.6f + vy * vy * 0.4f));
                row[x * 4 + 0] = clamp8(row[x * 4 + 0] * k);
                row[x * 4 + 1] = clamp8(row[x * 4 + 1] * k);
                row[x * 4 + 2] = clamp8(row[x * 4 + 2] * k);
                row[x * 4 + 3] = 255;
            }
        }
        tex = SDL_CreateTextureFromSurface(R, dst);
    }
    if (src) SDL_FreeSurface(src);
    if (dst) SDL_FreeSurface(dst);
    return tex;
}

// Banda de luz inclinada que cruza la pantalla despacio (una sola textura pequeña, un solo SDL_RenderCopy).
SDL_Texture* makeStreak() {
    SDL_Surface* s = newSurface(180, 360);
    if (!s || !s->pixels) {
        if (s) SDL_FreeSurface(s);
        return nullptr;
    }
    for (int y = 0; y < s->h; y++) {
        Uint8* row = (Uint8*)s->pixels + (size_t)y * (size_t)s->pitch;
        for (int x = 0; x < s->w; x++) {
            const float d = std::fabs((x - 90.f) + (y - 180.f) * 0.5f);
            float a = std::max(0.f, 1.f - d / 38.f);
            a *= a;
            row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
            row[x * 4 + 3] = clamp8(a * 255.f);
        }
    }
    SDL_Texture* t = SDL_CreateTextureFromSurface(R, s);
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

}  // namespace

void init(SDL_Renderer* r, const std::string& customPath) {
    shutdown();
    R = r;
    if (!R) return;
    try {
        g_bg = customBg(customPath);
        g_custom = g_bg != nullptr;
        if (!g_bg) g_bg = proceduralBg();
        g_streak = makeStreak();
    } catch (...) {
        util::log("tema: excepción al crear el fondo");
    }
    if (!customPath.empty()) util::log("fondo: %s", g_custom ? "imagen personalizada" : "por defecto (la imagen no se pudo usar)");
}

void shutdown() {
    if (g_bg) SDL_DestroyTexture(g_bg);
    if (g_streak) SDL_DestroyTexture(g_streak);
    g_bg = g_streak = nullptr;
    g_custom = false;
}

bool customBackground() { return g_custom; }

void background(uint64_t nowMs) {
    if (g_bg) {
        SDL_Rect dst = {0, 0, W, H};
        SDL_RenderCopy(R, g_bg, nullptr, &dst);
    }
    if (g_streak) {
        const int period = W + 720;
        const int x = (int)((nowMs / 10) % (uint64_t)period) - 360;
        SDL_SetTextureAlphaMod(g_streak, 20);
        SDL_Rect dst = {x, 0, 360, 720};
        SDL_RenderCopy(R, g_streak, nullptr, &dst);
    }
}

void slant(int x, int y, int w, int h, int skew, ui::Color c) {
    if (w <= 0 || h <= 0) return;
    if (skew == 0 || h == 1) {
        ui::fill(x, y, w, h, c);
        return;
    }
    for (int i = 0; i < h; i++) ui::fill(x + skew * (h - 1 - i) / (h - 1), y + i, w, 1, c);
}

void shadow(int x, int y, int w, int h, int depth) {
    for (int k = 1; k <= 3; k++) ui::fill(x - k * 2, y - k * 2 + depth, w + k * 4, h + k * 4, ui::Color(0, 0, 0, 26));
}

void panel(int x, int y, int w, int h, ui::Color accent) {
    shadow(x, y, w, h, 8);
    ui::fill(x, y, w, h, ui::col::panel);
    ui::fill(x, y, w, 3, accent.withAlpha(120));
    slant(x, y, std::min(w, 280), 9, 18, accent);
    ui::outline(x, y, w, h, ui::col::panelHi, 2);
}

void row(int x, int y, int w, int h, bool selected, ui::Color accent) {
    if (w <= 0 || h <= 0) return;
    ui::fill(x, y, w, h, selected ? ui::col::panelHi : ui::col::panel);
    ui::fill(x, y + h, w, 2, ui::Color(0, 0, 0, 70));  // sombra fina debajo
    if (selected) {
        ui::fill(x, y, w, 1, ui::Color(255, 255, 255, 34));
        ui::fill(x, y, 7, h, accent);
        static const uint8_t fade[6] = {34, 26, 19, 13, 8, 4};  // el tinte de la barra se apaga poco a poco hacia la derecha
        for (int k = 0; k < 6 && 7 + k * 22 < w; k++) ui::fill(x + 7 + k * 22, y, std::min(22, w - 7 - k * 22), h, accent.withAlpha(fade[k]));
    }
}

void subbar(int y, int h, ui::Color accent) {
    ui::fill(0, y, W, h, ui::Color(20, 23, 34, 235));
    ui::fill(0, y, 6, h, accent);
}

void glow(int x, int y, int w, int h, ui::Color c) {
    ui::outline(x - 7, y - 7, w + 14, h + 14, c.withAlpha(34), 3);
    ui::outline(x - 5, y - 5, w + 10, h + 10, c.withAlpha(86), 2);
    ui::outline(x - 3, y - 3, w + 6, h + 6, c, 3);
}

int chip(int x, int y, const std::string& key, bool primary) {
    const int w = std::max(28, ui::textWidth(key, ui::SMALL) + 18), h = 24;
    ui::fill(x, y + 2, w, h, ui::Color(0, 0, 0, 120));
    ui::fill(x, y, w, h, primary ? ui::col::accent : ui::col::panelHi);
    ui::fill(x, y, w, 2, ui::Color(255, 255, 255, 46));
    ui::text(key, x, y + (h - ui::lineHeight(ui::SMALL)) / 2, ui::SMALL, ui::col::text, w, 1);
    return w;
}

int hints(const std::string& line, int x, int y, int maxW, bool center) {
    const auto items = ui::splitHints(line);
    // medir primero (las medidas están en caché), para poder centrar
    std::vector<int> cw, lw;
    int total = 0;
    for (const auto& it : items) {
        const int c = std::max(28, ui::textWidth(it.first, ui::SMALL) + 18);
        const int l = it.second.empty() ? 0 : ui::textWidth(it.second, ui::SMALL);
        cw.push_back(c);
        lw.push_back(l);
        total += c + (l ? 8 + l : 0) + 22;
    }
    if (total > 0) total -= 22;
    int cx = center && total < maxW ? x + (maxW - total) / 2 : x;
    const int end = x + maxW;
    for (size_t i = 0; i < items.size(); i++) {
        const int need = cw[i] + (lw[i] ? 8 + lw[i] : 0);
        if (cx + need > end) break;  // lo que no cabe se omite entero (nunca a medias)
        chip(cx, y, items[i].first, items[i].first == "A");
        cx += cw[i];
        if (lw[i]) {
            ui::text(items[i].second, cx + 8, y + (24 - ui::lineHeight(ui::SMALL)) / 2, ui::SMALL, ui::col::dim);
            cx += 8 + lw[i];
        }
        cx += 22;
    }
    return std::min(total, maxW);
}

void equalizer(int x, int y, int w, int h, float level, uint64_t nowMs, bool on) {
    const int n = 4, gap = 3, bw = std::max(2, (w - gap * (n - 1)) / n);
    for (int k = 0; k < n; k++) {
        float amp = 0.10f;
        if (on) {
            const float wob = 0.55f + 0.45f * std::sin((float)nowMs * 0.006f * (1.f + 0.37f * k) + k * 1.7f);
            amp = 0.16f + 0.84f * std::min(1.f, level * 1.4f) * wob;
        }
        const int bh = std::max(3, (int)(h * amp));
        ui::fill(x + k * (bw + gap), y + h - bh, bw, bh, on ? ui::col::accent : ui::col::dim.withAlpha(120));
    }
}

}  // namespace theme
