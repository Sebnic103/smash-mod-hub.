// Prueba la lógica de texto de ui.cpp (ajuste de línea, caché con presupuesto por frame, LRU) con un SDL/TTF
// falso: cada carácter UTF-8 mide 10 px, la altura de línea es 20.
#include <SDL2/SDL_ttf.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../source/ui.hpp"

static int g_fail = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("  FALLO %s:%d  %s\n", __FILE__, __LINE__, #c);  \
            g_fail++;                                               \
        }                                                           \
    } while (0)

struct TTF_Font { int id; };
static int g_copies = 0, g_sizeCalls = 0, g_created = 0, g_destroyed = 0;
static bool g_clipOn = false;
static SDL_Rect g_clip = {0, 0, 0, 0};

SDL_RWops* SDL_RWFromMem(void*, int) { static int x; return (SDL_RWops*)&x; }
TTF_Font* TTF_OpenFontRW(SDL_RWops*, int, int) { static TTF_Font f[8]; static int n; return &f[n++ % 8]; }
void TTF_CloseFont(TTF_Font*) {}
static int cps(const char* s) {
    int n = 0;
    for (; *s; s++) n += (((unsigned char)*s) & 0xC0) != 0x80;
    return n;
}
static SDL_Surface g_surf;
SDL_Surface* TTF_RenderUTF8_Blended(TTF_Font*, const char* s, SDL_Color) { g_surf.w = 10 * cps(s); g_surf.h = 20; return &g_surf; }
SDL_Surface* TTF_RenderUTF8_Blended_Wrapped(TTF_Font*, const char* s, SDL_Color, Uint32) { g_surf.w = 10 * cps(s); g_surf.h = 20; return &g_surf; }
int TTF_SizeUTF8(TTF_Font*, const char* s, int* w, int* h) {
    g_sizeCalls++;
    *w = 10 * cps(s);
    *h = 20;
    return 0;
}
int TTF_FontLineSkip(const TTF_Font*) { return 20; }
int SDL_SetRenderDrawBlendMode(SDL_Renderer*, SDL_BlendMode) { return 0; }
int SDL_SetRenderDrawColor(SDL_Renderer*, Uint8, Uint8, Uint8, Uint8) { return 0; }
int SDL_RenderFillRect(SDL_Renderer*, const SDL_Rect*) { return 0; }
int SDL_RenderCopy(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*) { g_copies++; return 0; }
int SDL_RenderSetClipRect(SDL_Renderer*, const SDL_Rect* r) {
    g_clipOn = r != nullptr;
    if (r) g_clip = *r;
    return 0;
}
void SDL_RenderGetClipRect(SDL_Renderer*, SDL_Rect* r) { *r = g_clip; }
SDL_bool SDL_RenderIsClipEnabled(SDL_Renderer*) { return g_clipOn ? SDL_TRUE : SDL_FALSE; }
int SDL_SetTextureAlphaMod(SDL_Texture*, Uint8) { return 0; }
int SDL_QueryTexture(SDL_Texture*, Uint32*, int*, int*, int*) { return 0; }
SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer*, SDL_Surface*) {
    g_created++;
    return (SDL_Texture*)&g_surf;
}
void SDL_FreeSurface(SDL_Surface*) {}
void SDL_DestroyTexture(SDL_Texture*) { g_destroyed++; }

int main() {
    char font[16];
    CHECK(ui::init((SDL_Renderer*)&g_surf, font, sizeof font));
    for (int i = 0; i < 4; i++) ui::endFrame();  // abre presupuesto

    // ---- ajuste de línea
    CHECK(ui::textWrapped("", 0, 0, 100, ui::SMALL, ui::col::text) == 0);
    CHECK(ui::textWrapped("aaaa bbbb cccc", 0, 0, 90, ui::SMALL, ui::col::text) == 40);   // "aaaa bbbb" / "cccc"
    CHECK(ui::textWrapped("aaaa bbbb cccc", 0, 0, 89, ui::SMALL, ui::col::text) == 60);   // una palabra por línea
    CHECK(ui::textWrapped("a\n\nb", 0, 0, 100, ui::SMALL, ui::col::text) == 60);         // línea vacía en medio
    CHECK(ui::textWrapped("a\r\nb\r\n", 0, 0, 100, ui::SMALL, ui::col::text) == 60);      // "a","b","" (CRLF final)
    CHECK(ui::textWrapped(std::string(25, 'x'), 0, 0, 100, ui::SMALL, ui::col::text) == 60);  // 10+10+5
    CHECK(ui::textWrapped("\xC3\xB1\xC3\xB1\xC3\xB1\xC3\xB1\xC3\xB1", 0, 0, 30, ui::SMALL, ui::col::text) == 40);  // ñ x5 -> 3+2
    CHECK(ui::textWrapped("uno  dos   tres", 0, 0, 1000, ui::SMALL, ui::col::text) == 20);  // espacios repetidos
    // texto largo: 2000 palabras
    std::string big;
    for (int i = 0; i < 2000; i++) big += "palabra ";
    int th = ui::textWrapped(big, 0, 0, 400, ui::SMALL, ui::col::text);  // 5 palabras por línea (5*7+4=39 car.)
    CHECK(th == 20 * 400);
    // solo se dibuja lo visible
    ui::endFrame();
    g_copies = 0;
    ui::textWrapped(big, 0, 0, 400, ui::SMALL, ui::col::text, 20 * 10, 100);  // 5 líneas visibles
    CHECK(g_copies >= 5 && g_copies <= 6);
    CHECK(!g_clipOn);  // restaura el recorte

    // ---- recorte previo se conserva
    ui::clip(1, 2, 3, 4);
    ui::textWrapped("a b c", 0, 0, 100, ui::SMALL, ui::col::text, 0, 50);
    CHECK(g_clipOn && g_clip.x == 1 && g_clip.w == 3);
    ui::unclip();

    // ---- presupuesto por frame: 40 cadenas nuevas -> 16 por frame
    ui::endFrame();
    g_copies = 0;
    auto drawAll = [&](int n, int base) {
        for (int i = 0; i < n; i++) ui::text("cadena " + std::to_string(base + i), 0, i * 20, ui::SMALL, ui::col::text);
    };
    drawAll(40, 0);
    CHECK(g_copies == 16);
    ui::endFrame();
    g_copies = 0;
    drawAll(40, 0);
    CHECK(g_copies == 16 + 16);  // 16 ya en caché + 16 nuevas
    ui::endFrame();
    g_copies = 0;
    drawAll(40, 0);
    CHECK(g_copies == 32 + 8);
    ui::endFrame();
    g_copies = 0;
    drawAll(40, 0);
    CHECK(g_copies == 40);

    // ---- medir una vez: repetir el mismo texto no vuelve a llamar a TTF_SizeUTF8
    std::string longTxt(40, 'z');
    ui::text(longTxt, 0, 0, ui::SMALL, ui::col::text, 100);
    ui::endFrame();
    int before = g_sizeCalls;
    for (int i = 0; i < 10; i++) {
        ui::text(longTxt, 0, 0, ui::SMALL, ui::col::text, 100);
        ui::endFrame();
    }
    CHECK(g_sizeCalls == before);
    CHECK(ui::textWidth("hola", ui::SMALL) == 40);

    // ---- LRU: miles de cadenas distintas no hacen crecer la caché sin límite
    for (int f = 0; f < 300; f++) {
        drawAll(16, 1000 + f * 16);
        ui::endFrame();
    }
    CHECK(g_created - g_destroyed <= 900 + 16);
    CHECK(g_destroyed > 0);

    ui::shutdown();
    printf(g_fail ? "UI: %d FALLOS\n" : "UI: TODO OK\n", g_fail);
    return g_fail ? 1 : 0;
}
