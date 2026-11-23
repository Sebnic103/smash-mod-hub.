#pragma once
// Aspecto de la interfaz: fondo (generado una sola vez como textura, o una imagen tuya de la SD), formas
// inclinadas estilo "juego de lucha", sombras, paneles y chips de botones. Todo se dibuja con rectángulos y
// UNA textura de fondo: no crea texturas nuevas por frame, así que no cuesta rendimiento.
#include <SDL2/SDL.h>

#include <cstdint>
#include <string>

#include "ui.hpp"

namespace theme {

// customBg: ruta de una imagen propia (png/jpg) para el fondo; "" o ilegible = fondo por defecto.
void init(SDL_Renderer* r, const std::string& customBg);
void shutdown();
bool customBackground();

void background(uint64_t nowMs);

// Paralelogramo: el borde superior queda desplazado `skew` píxeles a la derecha.
void slant(int x, int y, int w, int h, int skew, ui::Color c);
void shadow(int x, int y, int w, int h, int depth = 6);
// Ventana modal: sombra, fondo, borde y una "lengüeta" de color arriba a la izquierda.
void panel(int x, int y, int w, int h, ui::Color accent);
// Fila de lista: fondo, barra de color a la izquierda y brillo fino arriba cuando está seleccionada.
void row(int x, int y, int w, int h, bool selected, ui::Color accent);
// Barra de información bajo la cabecera (con una marca de color a la izquierda).
void subbar(int y, int h, ui::Color accent);
// Resplandor alrededor de un elemento seleccionado.
void glow(int x, int y, int w, int h, ui::Color c);
// Recuadro de botón ("A", "ZL/ZR"...). Devuelve el ancho dibujado.
int chip(int x, int y, const std::string& key, bool primary = false);
// Línea de atajos ("A Ver   Y Buscar ...") como chips + texto. Devuelve el ancho usado.
int hints(const std::string& line, int x, int y, int maxW, bool center = false);
// Barritas de ecualizador (nivel 0..1); apagadas si !on.
void equalizer(int x, int y, int w, int h, float level, uint64_t nowMs, bool on);

}  // namespace theme
