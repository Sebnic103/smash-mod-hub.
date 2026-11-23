#include "ui.hpp"

#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ui {

namespace {

SDL_Renderer* R = nullptr;
TTF_Font* g_fonts[4] = {nullptr, nullptr, nullptr, nullptr};
const int kSizes[4] = {18, 22, 30, 46};

struct CachedText {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    uint64_t last = 0;  // último frame en que se usó
};
std::unordered_map<std::string, CachedText> g_cache;
uint64_t g_frame = 1;
int g_budget = 0;  // texturas de texto nuevas que aún se pueden crear en este frame
constexpr int kBudgetPerFrame = 16;
constexpr size_t kMaxCached = 900;

// Crear una textura de texto cuesta (render con FreeType + subida a la GPU). Al cambiar de página aparecen
// decenas de cadenas nuevas de golpe: si se hicieran todas en un frame, la interfaz se quedaba parada. Ahora
// se reparten entre varios frames (lo que falte se dibuja en el siguiente) y la caché se limpia con LRU en
// vez de borrarla entera.
CachedText getText(const std::string& s, FontSize f, Color c, int wrap) {
    if (s.empty() || !g_fonts[f]) return {};
    char hdr[48];
    snprintf(hdr, sizeof hdr, "%d|%02x%02x%02x|%d|", (int)f, c.r, c.g, c.b, wrap);
    std::string key = std::string(hdr) + s;
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        it->second.last = g_frame;
        return it->second;
    }
    if (g_budget <= 0) return {};
    g_budget--;
    SDL_Color sc = {c.r, c.g, c.b, 255};
    SDL_Surface* surf = wrap > 0 ? TTF_RenderUTF8_Blended_Wrapped(g_fonts[f], s.c_str(), sc, (Uint32)wrap) : TTF_RenderUTF8_Blended(g_fonts[f], s.c_str(), sc);
    if (!surf) return {};
    CachedText ct;
    ct.tex = SDL_CreateTextureFromSurface(R, surf);
    ct.w = surf->w;
    ct.h = surf->h;
    ct.last = g_frame;
    SDL_FreeSurface(surf);
    if (!ct.tex) return {};
    g_cache[key] = ct;
    return ct;
}

// Anchos de texto ya medidos (TTF_SizeUTF8 es lento y se pedía en cada frame para las mismas cadenas).
std::unordered_map<std::string, int> g_width;

int measure(const std::string& s, FontSize f) {
    if (s.empty() || !g_fonts[f]) return 0;
    std::string key(1, (char)('0' + (int)f));
    key += s;
    auto it = g_width.find(key);
    if (it != g_width.end()) return it->second;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(g_fonts[f], s.c_str(), &w, &h) != 0) w = 0;
    if (g_width.size() > 6000) g_width.clear();
    g_width[key] = w;
    return w;
}

std::unordered_map<std::string, std::string> g_ellipsis;

// Recorta una cadena UTF-8 para que quepa en maxW (añade "..."). El resultado se memoriza.
std::string ellipsize(const std::string& s, FontSize f, int maxW) {
    if (!g_fonts[f] || measure(s, f) <= maxW) return s;
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
        if (measure(t, f) <= maxW) {
            g_ellipsis[key] = t;
            return t;
        }
    }
    g_ellipsis[key] = "...";
    return "...";
}

// ---- ajuste de línea propio: se dibuja solo lo visible, línea a línea (antes, una textura gigante por
// descripción: renderizar 4000 caracteres de golpe tardaba decenas de ms).
std::unordered_map<std::string, std::vector<std::string>> g_wrap;

void hardBreak(const std::string& word, FontSize f, int w, std::vector<std::string>& out, std::string& rest) {
    std::string piece;
    size_t i = 0;
    while (i < word.size()) {
        size_t n = 1;
        while (i + n < word.size() && (((unsigned char)word[i + n]) & 0xC0) == 0x80) n++;
        std::string next = piece + word.substr(i, n);
        if (!piece.empty() && measure(next, f) > w) {
            out.push_back(piece);
            piece = word.substr(i, n);
        } else {
            piece = next;
        }
        i += n;
    }
    rest = piece;
}

void wrapParagraph(const std::string& para, FontSize f, int w, std::vector<std::string>& out) {
    const int sp = measure(" ", f);
    std::string line;
    int lineW = 0;
    size_t pos = 0;
    while (pos < para.size()) {
        size_t e = para.find(' ', pos);
        if (e == std::string::npos) e = para.size();
        std::string word = para.substr(pos, e - pos);
        pos = e + 1;
        if (word.empty()) continue;
        int ww = measure(word, f);
        if (!line.empty() && lineW + sp + ww <= w) {
            line += ' ';
            line += word;
            lineW += sp + ww;
            continue;
        }
        if (!line.empty()) {
            out.push_back(line);
            line.clear();
            lineW = 0;
        }
        if (ww <= w) {
            line = word;
            lineW = ww;
        } else {
            hardBreak(word, f, w, out, line);
            lineW = measure(line, f);
        }
    }
    if (!line.empty()) out.push_back(line);
}

const std::vector<std::string>& wrapLines(const std::string& s, FontSize f, int w) {
    char hdr[32];
    snprintf(hdr, sizeof hdr, "%d|%d|", (int)f, w);
    const std::string key = std::string(hdr) + s;
    auto it = g_wrap.find(key);
    if (it != g_wrap.end()) return it->second;
    if (g_wrap.size() > 12) g_wrap.clear();
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        std::string para = s.substr(pos, nl - pos);
        if (!para.empty() && para.back() == '\r') para.pop_back();
        if (para.empty()) out.push_back("");
        else wrapParagraph(para, f, w, out);
        pos = nl + 1;
    }
    return g_wrap.emplace(key, std::move(out)).first->second;
}

}  // namespace

bool init(SDL_Renderer* renderer, void* fontData, size_t fontSize) {
    R = renderer;
    g_budget = kBudgetPerFrame;
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
    g_wrap.clear();
}

void endFrame() {
    g_frame++;
    g_budget = kBudgetPerFrame;
    if (g_cache.size() <= kMaxCached) return;
    // Demasiadas texturas de texto: fuera las que más tiempo llevan sin usarse (hasta quedar al 75%).
    std::vector<std::pair<uint64_t, std::string>> order;
    order.reserve(g_cache.size());
    for (auto& kv : g_cache) order.emplace_back(kv.second.last, kv.first);
    std::sort(order.begin(), order.end());
    size_t drop = g_cache.size() - kMaxCached * 3 / 4;
    for (size_t i = 0; i < drop && i < order.size(); i++) {
        if (order[i].first + 2 >= g_frame) break;  // lo de los últimos frames no se toca
        auto it = g_cache.find(order[i].second);
        if (it == g_cache.end()) continue;
        if (it->second.tex) SDL_DestroyTexture(it->second.tex);
        g_cache.erase(it);
    }
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

int textWidth(const std::string& s, FontSize f) { return measure(s, f); }

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
    if (s.empty() || !g_fonts[f]) return 0;
    const std::vector<std::string>& lines = wrapLines(s, f, w);
    const int lh = lineHeight(f);
    const int total = (int)lines.size() * lh;
    SDL_Rect prev = {0, 0, 0, 0};
    bool hadClip = false;
    if (maxH > 0) {
        hadClip = SDL_RenderIsClipEnabled(R) == SDL_TRUE;
        if (hadClip) SDL_RenderGetClipRect(R, &prev);
        clip(x, y, w, maxH);
    }
    const int first = maxH > 0 ? std::max(0, skipY / std::max(lh, 1)) : 0;
    for (int i = first; i < (int)lines.size(); i++) {
        int ly = y + i * lh - (maxH > 0 ? skipY : 0);
        if (maxH > 0 && ly >= y + maxH) break;
        if (!lines[(size_t)i].empty()) text(lines[(size_t)i], x, ly, f, c);
    }
    if (maxH > 0) {
        if (hadClip) SDL_RenderSetClipRect(R, &prev);
        else unclip();
    }
    return total;
}

void scrollbar(int x, int y, int h, int viewH, int contentH, int pos, Color c) {
    if (contentH <= viewH || h <= 0) return;
    fill(x, y, 4, h, col::panel);
    int th = std::max(28, (int)((int64_t)h * viewH / contentH));
    int maxPos = contentH - viewH;
    int ty = y + (int)((int64_t)(h - th) * std::min(std::max(pos, 0), maxPos) / maxPos);
    fill(x, ty, 4, th, c);
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

std::vector<std::pair<std::string, std::string>> splitHints(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find("   ", pos);
        const size_t end = e == std::string::npos ? s.size() : e;
        std::string seg = s.substr(pos, end - pos);
        size_t a = seg.find_first_not_of(' ');
        if (a != std::string::npos) {
            seg = seg.substr(a);
            const size_t sp = seg.find(' ');
            if (sp == std::string::npos) {
                out.emplace_back(seg, "");
            } else {
                const size_t b = seg.find_first_not_of(' ', sp);
                out.emplace_back(seg.substr(0, sp), b == std::string::npos ? "" : seg.substr(b));
            }
        }
        if (e == std::string::npos) break;
        pos = s.find_first_not_of(' ', e);
        if (pos == std::string::npos) break;
    }
    return out;
}

void clip(int x, int y, int w, int h) {
    SDL_Rect r = {x, y, w, h};
    SDL_RenderSetClipRect(R, &r);
}

void unclip() { SDL_RenderSetClipRect(R, nullptr); }

}  // namespace ui
