#pragma once
#include <SDL2/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ui {

struct Color {
    uint8_t r, g, b, a;
    constexpr Color(uint8_t r_ = 0, uint8_t g_ = 0, uint8_t b_ = 0, uint8_t a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}
    Color withAlpha(uint8_t na) const { return Color(r, g, b, na); }
};

namespace col {
constexpr Color bg(16, 18, 26);
constexpr Color header(10, 11, 17);
constexpr Color panel(28, 32, 46);
constexpr Color panelHi(42, 48, 68);
constexpr Color placeholder(36, 40, 56);
constexpr Color text(236, 239, 246);
constexpr Color dim(150, 158, 178);
constexpr Color accent(233, 69, 96);
constexpr Color good(72, 190, 120);
constexpr Color warn(240, 178, 58);
constexpr Color bad(226, 84, 84);
constexpr Color white(255, 255, 255);
}  // namespace col

enum FontSize { SMALL = 0, NORMAL = 1, BIG = 2, HUGE_ = 3 };

// fontData/fontSize: fuente TTF en memoria (la fuente compartida de la consola).
bool init(SDL_Renderer* renderer, void* fontData, size_t fontSize);
void shutdown();

void fill(int x, int y, int w, int h, Color c);
void outline(int x, int y, int w, int h, Color c, int thickness = 2);

int textWidth(const std::string& s, FontSize f);
int lineHeight(FontSize f);
// Dibuja una línea. Si maxW > 0 recorta con "..." y align (0 izq, 1 centro, 2 der) actúa dentro de maxW.
void text(const std::string& s, int x, int y, FontSize f, Color c, int maxW = 0, int align = 0);
// Texto con saltos de línea automáticos. Dibuja solo la franja [skipY, skipY+maxH) y devuelve la altura total.
int textWrapped(const std::string& s, int x, int y, int w, FontSize f, Color c, int skipY = 0, int maxH = 0);

void image(SDL_Texture* t, int x, int y, int w, int h);         // rellena recortando (cover)
void imageContain(SDL_Texture* t, int x, int y, int w, int h);  // entra entera (contain)

// Barra de desplazamiento fina (x,y,h = pista; viewH/contentH/pos en píxeles del contenido).
void scrollbar(int x, int y, int h, int viewH, int contentH, int pos, Color c = col::dim);

void clip(int x, int y, int w, int h);
void unclip();
void clearTextCache();
// Llamar al final de cada frame: reparte la creación de texturas de texto y limpia la caché (LRU).
void endFrame();

}  // namespace ui
