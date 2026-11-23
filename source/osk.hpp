#pragma once
// Teclado en pantalla propio. Sustituye al teclado del sistema (swkbd): ese teclado es una "applet" que se lanza
// de forma síncrona desde el hilo principal, y si el sistema tarda, no responde o la app corre en modo applet
// (desde el Álbum), la consola se queda congelada hasta reiniciarla. Este teclado es una capa más de la interfaz:
// no bloquea nada, se maneja con mando o con la pantalla táctil y se prueba en el PC (tests/test_osk.cpp).
//
// Núcleo sin SDL ni libnx: solo lógica de texto, teclas y geometría; app.cpp lo dibuja.
#include <string>
#include <vector>

#include "input.hpp"

namespace osk {

enum class Result { None, Accepted, Cancelled };
enum class Kind { Char, Shift, Symbols, Space, Back, Cancel, Accept };

struct Key {
    Kind kind = Kind::Char;
    std::string label;  // solo teclas de carácter (los nombres de las especiales los pone la interfaz, según idioma)
    std::string out;
    int w = 2;  // ancho relativo
};

struct Box {
    int row = 0, col = 0;
    int x = 0, y = 0, w = 0, h = 0;
};

struct Geometry {
    int x = 0, y = 0, w = 0, rowH = 0, gap = 0;
};

class Keyboard {
public:
    void open(const std::string& title, const std::string& initial, int maxLen, bool numeric);
    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    // Procesa la entrada de un frame. Al devolver Accepted o Cancelled el teclado ya queda cerrado; el texto
    // final sigue disponible en text().
    Result update(const Input& in);

    const std::string& title() const { return title_; }
    const std::string& text() const { return text_; }
    std::string beforeCursor() const { return text_.substr(0, cur_); }
    int length() const;  // caracteres (no bytes)
    int maxLen() const { return maxLen_; }
    bool numeric() const { return numeric_; }
    bool shift() const { return shift_; }
    bool symbols() const { return page_ == 1; }
    int selRow() const { return row_; }
    int selCol() const { return col_; }
    const std::vector<std::vector<Key>>& rows() const { return rows_; }

    // Lo que se ve en la tecla (aplica mayúsculas si Mayús está activa).
    std::string shown(const Key& k) const;
    Geometry geometry() const;
    std::vector<Box> layout() const;  // misma geometría para dibujar y para los toques

    // Utilidades de texto UTF-8 (públicas para los tests)
    static int countChars(const std::string& s);
    static std::string upper(const std::string& ch);

private:
    void rebuild();
    void clampSel();
    void moveH(int d);
    void moveV(int d);
    void memorize();
    float centerOf(int row, int col) const;
    Result press(const Key& k);
    void insert(const std::string& s);
    void backspace();
    void cursorLeft();
    void cursorRight();

    bool open_ = false, numeric_ = false, shift_ = false;
    int page_ = 0, maxLen_ = 60, row_ = 0, col_ = 0;
    float memX_ = 0.f;  // posición horizontal (0..1) que se intenta conservar al subir y bajar
    size_t cur_ = 0;    // posición del cursor, en bytes (siempre en el límite de un carácter)
    std::string title_, text_;
    std::vector<std::vector<Key>> rows_;
};

}  // namespace osk
