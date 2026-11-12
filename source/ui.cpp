#include "ui.hpp"

#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace ui {

namespace {

SDL_Renderer* R = nullptr;
TTF_Font* g_fonts[4] = {nullptr, nullptr, nullptr, nullptr};
const int kSizes[4] = {18, 22, 30, 46};

struct CachedText {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
};
std::unordered_map<std::string, CachedText> g_cache;

CachedText getText(const std::string& s, FontSize f, Color c, int wrap) {
    if (s.empty() || !g_fonts[f]) return {};
    char hdr[48];
    snprintf(hdr, sizeof hdr, "%d|%02x%02x%02x|%d|", (int)f, c.r, c.g, c.b, wrap);
    std::string key = std::string(hdr) + s;
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    if (g_cache.size() > 700) clearTextCache();
    SDL_Color sc = {c.r, c.g, c.b, 255};
    SDL_Surface* surf = wrap > 0 ? TTF_RenderUTF8_Blended_Wrapped(g_fonts[f], s.c_str(), sc, (Uint32)wrap) : TTF_RenderUTF8_Blended(g_fonts[f], s.c_str(), sc);
    if (!surf) return {};
    CachedText ct;
    ct.tex = SDL_CreateTextureFromSurface(R, surf);
    ct.w = surf->w;
    ct.h = surf->h;
    SDL_FreeSurface(surf);
    if (!ct.tex) return {};
    g_cache[key] = ct;
    return ct;
}

std::unordered_map<std::string, std::string> g_ellipsis;

// Recorta una cadena UTF-8 para que quepa en maxW (añade "..."). El resultado se memoriza:
// se pide en cada frame para las mismas cadenas y medir con TTF es caro en la consola.
std::string ellipsize(const std::string& s, FontSize f, int maxW) {
    int w = 0, h = 0;
    if (!g_fonts[f] || TTF_SizeUTF8(g_fonts[f], s.c_str(), &w, &h) != 0 || w <= maxW) return s;
    char hdr[32];
    snprintf(hdr, sizeof hdr, "%d|%d|", (int)f, maxW);
    const std::string key = std::string(hdr) + s;
    auto cached = g_ellipsis.find(key);
    if (cached != g_ellipsis.end()) return cached->second;
    if (g_ellipsis.size() > 2000) g_ellipsis.clear();
    std::string out = s;
    while (!out.empty()) {
        // quita un carácter UTF-8 completo
        size_t n = out.size() - 1;
        while (n > 0 && (((unsigned char)out[n]) & 0xC0) == 0x80) n--;
        out.resize(n);
        std::string t = out + "...";
        if (TTF_SizeUTF8(g_fonts[f], t.c_str(), &w, &h) == 0 && w <= maxW) {
            g_ellipsis[key] = t;
            return t;
        }
    }
    g_ellipsis[key] = "...";
    return "...";
}

}  // namespace

bool init(SDL_Renderer* renderer, void* fontData, size_t fontSize) {
    R = renderer;
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 4; i++) {
        SDL_RWops* rw = SDL_RWFromMem(fontData, (int)fontSize);
        g_fonts[i] = rw ? TTF_OpenFontRW(rw, 1, kSizes[i]) : nullptr;  // 1: TTF cierra el RWops con la fuente
        if (!g_fonts[i]) return false;
    }
    return true;
}

void shutdown() {
    clearTextCache();
    for (auto& f : g_fonts) {
        if (f) TTF_CloseFont(f);
        f = nullptr;
    }
}

void clearTextCache() {
    for (auto& kv : g_cache)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    g_cache.clear();
}

void fill(int x, int y, int w, int h, Color c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(R, &r);
}

void outline(int x, int y, int w, int h, Color c, int t) {
    fill(x, y, w, t, c);
    fill(x, y + h - t, w, t, c);
    fill(x, y, t, h, c);
    fill(x + w - t, y, t, h, c);
}

int textWidth(const std::string& s, FontSize f) {
    int w = 0, h = 0;
    if (s.empty() || !g_fonts[f]) return 0;
    TTF_SizeUTF8(g_fonts[f], s.c_str(), &w, &h);
    return w;
}

int lineHeight(FontSize f) { return g_fonts[f] ? TTF_FontLineSkip(g_fonts[f]) : kSizes[f]; }

void text(const std::string& s, int x, int y, FontSize f, Color c, int maxW, int align) {
    std::string str = maxW > 0 ? ellipsize(s, f, maxW) : s;
    CachedText ct = getText(str, f, c, 0);
    if (!ct.tex) return;
    int dx = x;
    if (maxW > 0 && align == 1) dx = x + (maxW - ct.w) / 2;
    else if (maxW > 0 && align == 2) dx = x + maxW - ct.w;
    SDL_SetTextureAlphaMod(ct.tex, c.a);
    SDL_Rect dst = {dx, y, ct.w, ct.h};
    SDL_RenderCopy(R, ct.tex, nullptr, &dst);
}

int textWrapped(const std::string& s, int x, int y, int w, FontSize f, Color c, int skipY, int maxH) {
    CachedText ct = getText(s, f, c, w);
    if (!ct.tex) return 0;
    if (maxH > 0) {
        int sy = std::min(skipY, std::max(0, ct.h - 1));
        int hh = std::min(maxH, ct.h - sy);
        if (hh > 0) {
            SDL_Rect src = {0, sy, ct.w, hh};
            SDL_Rect dst = {x, y, ct.w, hh};
            SDL_SetTextureAlphaMod(ct.tex, c.a);
            SDL_RenderCopy(R, ct.tex, &src, &dst);
        }
    } else {
        SDL_Rect dst = {x, y, ct.w, ct.h};
        SDL_RenderCopy(R, ct.tex, nullptr, &dst);
    }
    return ct.h;
}

void image(SDL_Texture* t, int x, int y, int w, int h) {
    if (!t || w <= 0 || h <= 0) return;
    int tw = 0, th = 0;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    double sa = (double)tw / th, da = (double)w / h;
    SDL_Rect src;
    if (sa > da) {
        src.h = th;
        src.w = (int)(th * da);
        src.x = (tw - src.w) / 2;
        src.y = 0;
    } else {
        src.w = tw;
        src.h = (int)(tw / da);
        src.x = 0;
        src.y = (th - src.h) / 2;
    }
    SDL_Rect dst = {x, y, w, h};
    SDL_RenderCopy(R, t, &src, &dst);
}

void imageContain(SDL_Texture* t, int x, int y, int w, int h) {
    if (!t || w <= 0 || h <= 0) return;
    int tw = 0, th = 0;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    double k = std::min((double)w / tw, (double)h / th);
    int dw = (int)(tw * k), dh = (int)(th * k);
    SDL_Rect dst = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_RenderCopy(R, t, nullptr, &dst);
}

void clip(int x, int y, int w, int h) {
    SDL_Rect r = {x, y, w, h};
    SDL_RenderSetClipRect(R, &r);
}

void unclip() { SDL_RenderSetClipRect(R, nullptr); }

}  // namespace ui
