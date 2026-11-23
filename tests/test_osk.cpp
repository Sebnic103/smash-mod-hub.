// Pruebas del teclado en pantalla (osk.cpp): escritura, cursor, UTF-8, navegación, límites y toques.
#include <cstdio>
#include <string>

#include "../source/osk.hpp"

static int g_fail = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("  FALLO %s:%d  %s\n", __FILE__, __LINE__, #c);  \
            g_fail++;                                               \
        }                                                           \
    } while (0)

using osk::Keyboard;
using osk::Result;

static Input A() { Input i; i.a = true; return i; }
static Input dir(char d) {
    Input i;
    if (d == 'u') i.up = true;
    if (d == 'd') i.down = true;
    if (d == 'l') i.left = true;
    if (d == 'r') i.right = true;
    return i;
}

// Mueve la selección hasta la tecla cuyo texto es "s" (busca fila a fila) y la pulsa.
static Result typeKey(Keyboard& k, const std::string& s) {
    const auto& rows = k.rows();
    for (int r = 0; r < (int)rows.size(); r++)
        for (int c = 0; c < (int)rows[(size_t)r].size(); c++)
            if (rows[(size_t)r][(size_t)c].kind == osk::Kind::Char && rows[(size_t)r][(size_t)c].label == s) {
                for (const auto& b : k.layout())
                    if (b.row == r && b.col == c) {
                        Input t;
                        t.tap = true;
                        t.tapX = b.x + b.w / 2;
                        t.tapY = b.y + b.h / 2;
                        return k.update(t);
                    }
            }
    return Result::None;
}

static void pressSpecial(Keyboard& k, osk::Kind kind, Result expect) {
    for (const auto& b : k.layout()) {
        if (k.rows()[(size_t)b.row][(size_t)b.col].kind == kind) {
            Input t;
            t.tap = true;
            t.tapX = b.x + b.w / 2;
            t.tapY = b.y + b.h / 2;
            CHECK(k.update(t) == expect);
            return;
        }
    }
    CHECK(false);
}

static void testTyping() {
    Keyboard k;
    k.open("Buscar", "", 50, false);
    CHECK(k.isOpen());
    CHECK(typeKey(k, "m") == Result::None);
    CHECK(typeKey(k, "a") == Result::None);
    CHECK(typeKey(k, "r") == Result::None);
    CHECK(k.text() == "mar");
    // Mayús: una sola letra y vuelve a minúsculas
    Input y;
    y.y = true;
    k.update(y);
    CHECK(k.shift());
    typeKey(k, "i");
    CHECK(!k.shift());
    typeKey(k, "o");
    CHECK(k.text() == "marIo");
    // espacio con X, borrar con B (repetición)
    Input x;
    x.x = true;
    k.update(x);
    CHECK(k.text() == "marIo ");
    Input b;
    b.bRep = true;
    k.update(b);
    k.update(b);
    CHECK(k.text() == "marI");
    // B también pulsa la tecla Borrar en pantalla
    pressSpecial(k, osk::Kind::Back, Result::None);
    CHECK(k.text() == "mar");
    // aceptar con + cierra y conserva el texto
    Input plus;
    plus.plus = true;
    CHECK(k.update(plus) == Result::Accepted);
    CHECK(!k.isOpen());
    CHECK(k.text() == "mar");
    CHECK(k.update(A()) == Result::None);  // cerrado: ya no hace nada
}

static void testCancel() {
    Keyboard k;
    k.open("t", "abc", 10, false);
    Input m;
    m.minus = true;
    CHECK(k.update(m) == Result::Cancelled);
    CHECK(!k.isOpen());
    k.open("t", "abc", 10, false);
    pressSpecial(k, osk::Kind::Cancel, Result::Cancelled);
    k.open("t", "abc", 10, false);
    pressSpecial(k, osk::Kind::Accept, Result::Accepted);
    CHECK(k.text() == "abc");
}

static void testUtf8AndCursor() {
    Keyboard k;
    k.open("t", "a\xC3\xB1" "o", 10, false);  // "año"
    CHECK(k.length() == 3);
    Input l;
    l.l = true;
    k.update(l);  // antes de la o
    k.update(l);  // antes de la ñ
    Input bs;
    bs.bRep = true;
    k.update(bs);  // borra la 'a'
    CHECK(k.text() == "\xC3\xB1" "o");
    CHECK(k.beforeCursor().empty());
    typeKey(k, "x");
    CHECK(k.text() == "x\xC3\xB1" "o");
    Input r;
    r.r = true;
    k.update(r);
    k.update(bs);  // borra la ñ completa (2 bytes), no la mitad
    CHECK(k.text() == "xo");
    // más allá de los extremos no pasa nada
    Input zl, zr;
    zl.zl = true;
    zr.zr = true;
    k.update(zl);
    k.update(bs);
    CHECK(k.text() == "xo");
    k.update(zr);
    k.update(r);
    typeKey(k, "z");
    CHECK(k.text() == "xoz");
    // Mayús sobre una letra acentuada del teclado de símbolos
    pressSpecial(k, osk::Kind::Symbols, Result::None);
    CHECK(k.symbols());
    Input y;
    y.y = true;
    k.update(y);
    typeKey(k, "\xC3\xA1");  // á
    CHECK(k.text() == "xoz\xC3\x81");  // Á
    CHECK(Keyboard::upper("\xC3\xB7") == "\xC3\xB7");  // ÷ no cambia
    CHECK(Keyboard::upper("\xC3\xB1") == "\xC3\x91");
    CHECK(Keyboard::upper("7") == "7");
}

static void testLimits() {
    Keyboard k;
    k.open("t", "", 3, false);
    for (int i = 0; i < 10; i++) typeKey(k, "a");
    CHECK(k.text() == "aaa");
    Input x;
    x.x = true;
    k.update(x);
    CHECK(k.text() == "aaa");
    // texto inicial más largo que el máximo: se recorta sin partir caracteres
    k.open("t", "\xC3\xB1\xC3\xB1\xC3\xB1\xC3\xB1\xC3\xB1", 2, false);
    CHECK(k.text() == "\xC3\xB1\xC3\xB1");
    // máximo absurdo
    k.open("t", "", 0, false);
    CHECK(k.maxLen() == 1);
    k.open("t", "", 100000, false);
    CHECK(k.maxLen() == 200);
}

static void testNumeric() {
    Keyboard k;
    k.open("ID", "6498", 8, true);
    CHECK(k.numeric());
    CHECK(k.rows().size() == 5);
    Input bs;
    bs.bRep = true;
    k.update(bs);
    CHECK(k.text() == "649");
    typeKey(k, "1");
    CHECK(k.text() == "6491");
    Input y;
    y.y = true;
    k.update(y);
    CHECK(!k.shift());  // en el teclado numérico Mayús no existe
    Input plus;
    plus.plus = true;
    CHECK(k.update(plus) == Result::Accepted);
}

static void testNavigation() {
    Keyboard k;
    k.open("t", "", 10, false);
    CHECK(k.selRow() == 0 && k.selCol() == 0);
    k.update(dir('l'));  // da la vuelta
    CHECK(k.selCol() == 9);
    k.update(dir('r'));
    CHECK(k.selCol() == 0);
    k.update(dir('u'));  // de la primera fila a la última (da la vuelta)
    CHECK(k.selRow() == 4);
    k.update(dir('d'));
    CHECK(k.selRow() == 0);
    // bajar 4 filas y subir: vuelve a la misma columna (memoria horizontal)
    for (int i = 0; i < 6; i++) k.update(dir('r'));
    const int col = k.selCol();
    CHECK(col == 6);
    for (int i = 0; i < 4; i++) k.update(dir('d'));
    CHECK(k.selRow() == 4);  // fila de teclas especiales: hay menos teclas, pero la selección es válida
    CHECK(k.selCol() >= 0 && k.selCol() < (int)k.rows()[4].size());
    for (int i = 0; i < 4; i++) k.update(dir('u'));
    CHECK(k.selRow() == 0 && k.selCol() == col);
    // A sobre una tecla de carácter la escribe
    k.update(A());
    CHECK(k.text() == "7");
    // pasar a símbolos con la selección en la fila inferior no deja la selección fuera de rango
    for (int i = 0; i < 4; i++) k.update(dir('d'));
    for (int i = 0; i < 10; i++) {
        k.update(dir('r'));
        k.update(dir('d'));
        k.update(dir('l'));
        k.update(dir('u'));
        CHECK(k.selRow() >= 0 && k.selRow() < (int)k.rows().size());
        CHECK(k.selCol() >= 0 && k.selCol() < (int)k.rows()[(size_t)k.selRow()].size());
    }
}

static void testGeometry() {
    for (int numeric = 0; numeric < 2; numeric++) {
        Keyboard k;
        k.open("t", "", 10, numeric != 0);
        const auto g = k.geometry();
        const auto boxes = k.layout();
        size_t expect = 0;
        for (const auto& r : k.rows()) expect += r.size();
        CHECK(boxes.size() == expect);
        for (const auto& b : boxes) {
            CHECK(b.w > 0 && b.h > 0);
            CHECK(b.x >= g.x && b.x + b.w <= g.x + g.w);
            CHECK(b.y >= g.y);
            CHECK(b.y + b.h <= 720 - 44);  // cabe encima de la barra inferior
        }
        // sin solapes dentro de una fila
        for (size_t i = 1; i < boxes.size(); i++)
            if (boxes[i].row == boxes[i - 1].row) CHECK(boxes[i].x >= boxes[i - 1].x + boxes[i - 1].w);
    }
    // un toque fuera de cualquier tecla no hace nada
    Keyboard k;
    k.open("t", "", 10, false);
    Input t;
    t.tap = true;
    t.tapX = 5;
    t.tapY = 5;
    CHECK(k.update(t) == Result::None);
    CHECK(k.text().empty());
}

int main() {
    testTyping();
    testCancel();
    testUtf8AndCursor();
    testLimits();
    testNumeric();
    testNavigation();
    testGeometry();
    if (g_fail) {
        printf("test_osk: %d FALLOS\n", g_fail);
        return 1;
    }
    printf("test_osk: todo OK\n");
    return 0;
}
