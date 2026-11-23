#include "osk.hpp"

#include <algorithm>
#include <cmath>

namespace osk {

namespace {

Key ch(const char* s, int w = 2) {
    Key k;
    k.kind = Kind::Char;
    k.label = k.out = s;
    k.w = w;
    return k;
}

Key sp(Kind kind, int w) {
    Key k;
    k.kind = kind;
    k.w = w;
    return k;
}

std::vector<Key> charRow(const std::vector<const char*>& cs) {
    std::vector<Key> r;
    for (const char* c : cs) r.push_back(ch(c));
    return r;
}

int rowTotal(const std::vector<Key>& r) {
    int t = 0;
    for (const auto& k : r) t += k.w;
    return t;
}

size_t charLen(const std::string& s, size_t i) {  // bytes del carácter UTF-8 que empieza en i
    size_t n = 1;
    while (i + n < s.size() && (((unsigned char)s[i + n]) & 0xC0) == 0x80) n++;
    return n;
}

}  // namespace

int Keyboard::countChars(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// a-z, y las letras acentuadas de Latin-1 (á é í ó ú ü ñ...: C3 A0..BE salvo ÷) -> mayúscula
std::string Keyboard::upper(const std::string& c) {
    if (c.size() == 1 && c[0] >= 'a' && c[0] <= 'z') return std::string(1, (char)(c[0] - 32));
    if (c.size() == 2 && (unsigned char)c[0] == 0xC3) {
        const unsigned char b = (unsigned char)c[1];
        if (b >= 0xA0 && b <= 0xBE && b != 0xB7) {
            std::string o = c;
            o[1] = (char)(b - 0x20);
            return o;
        }
    }
    return c;
}

void Keyboard::open(const std::string& title, const std::string& initial, int maxLen, bool numeric) {
    open_ = true;
    title_ = title;
    numeric_ = numeric;
    maxLen_ = std::max(1, std::min(maxLen, 200));
    shift_ = false;
    page_ = 0;
    text_.clear();
    int n = 0;
    for (size_t i = 0; i < initial.size() && n < maxLen_;) {  // recorta sin partir un carácter
        const size_t l = charLen(initial, i);
        text_ += initial.substr(i, l);
        i += l;
        n++;
    }
    cur_ = text_.size();
    rebuild();
    row_ = 0;
    col_ = 0;
    memX_ = 0.f;
    clampSel();
    memorize();
}

void Keyboard::rebuild() {
    rows_.clear();
    if (numeric_) {
        rows_.push_back(charRow({"7", "8", "9"}));
        rows_.push_back(charRow({"4", "5", "6"}));
        rows_.push_back(charRow({"1", "2", "3"}));
        rows_.push_back({ch("0"), sp(Kind::Back, 2), sp(Kind::Accept, 2)});
        rows_.push_back({sp(Kind::Cancel, 6)});
        return;
    }
    if (page_ == 0) {
        rows_.push_back(charRow({"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"}));
        rows_.push_back(charRow({"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"}));
        rows_.push_back(charRow({"a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC3\xB1"}));
        rows_.push_back(charRow({"z", "x", "c", "v", "b", "n", "m", "-", "_", "."}));
    } else {
        rows_.push_back(charRow({"!", "@", "#", "$", "%", "&", "*", "(", ")", "/"}));
        rows_.push_back(charRow({"\xC3\xA1", "\xC3\xA9", "\xC3\xAD", "\xC3\xB3", "\xC3\xBA", "\xC3\xBC", "\xC3\xB1", "\xC2\xBF", "\xC2\xA1", "+"}));
        rows_.push_back(charRow({"?", ":", ";", "'", "\"", "=", "<", ">", "[", "]"}));
        rows_.push_back(charRow({",", ".", "-", "_", "~", "^", "|", "\\", "{", "}"}));
    }
    rows_.push_back({sp(Kind::Shift, 3), sp(Kind::Symbols, 3), sp(Kind::Space, 5), sp(Kind::Back, 3), sp(Kind::Cancel, 3), sp(Kind::Accept, 3)});
}

int Keyboard::length() const { return countChars(text_); }

std::string Keyboard::shown(const Key& k) const {
    if (k.kind == Kind::Char && shift_) return upper(k.label);
    return k.label;
}

Geometry Keyboard::geometry() const {
    Geometry g;
    if (numeric_) {
        g.x = 430;
        g.w = 420;
        g.rowH = 62;
    } else {
        g.x = 140;
        g.w = 1000;
        g.rowH = 62;
    }
    g.y = 240;
    g.gap = 8;
    return g;
}

std::vector<Box> Keyboard::layout() const {
    std::vector<Box> out;
    const Geometry g = geometry();
    for (int r = 0; r < (int)rows_.size(); r++) {
        const auto& row = rows_[(size_t)r];
        const int total = std::max(1, rowTotal(row));
        const int avail = g.w - g.gap * ((int)row.size() - 1);
        int x = g.x;
        for (int c = 0; c < (int)row.size(); c++) {
            const int w = avail * row[(size_t)c].w / total;
            Box b;
            b.row = r;
            b.col = c;
            b.x = x;
            b.y = g.y + r * (g.rowH + g.gap);
            b.w = (c == (int)row.size() - 1) ? g.x + g.w - x : w;  // la última llena el resto (sin huecos por redondeo)
            b.h = g.rowH;
            out.push_back(b);
            x += w + g.gap;
        }
    }
    return out;
}

float Keyboard::centerOf(int row, int col) const {
    const auto& r = rows_[(size_t)row];
    const int total = std::max(1, rowTotal(r));
    int before = 0;
    for (int i = 0; i < col; i++) before += r[(size_t)i].w;
    return (before + r[(size_t)col].w * 0.5f) / (float)total;
}

void Keyboard::memorize() {
    if (rows_.empty()) return;
    memX_ = centerOf(row_, col_);
}

void Keyboard::clampSel() {
    if (rows_.empty()) {
        row_ = col_ = 0;
        return;
    }
    row_ = std::max(0, std::min(row_, (int)rows_.size() - 1));
    col_ = std::max(0, std::min(col_, (int)rows_[(size_t)row_].size() - 1));
}

void Keyboard::moveH(int d) {
    const int n = (int)rows_[(size_t)row_].size();
    col_ = ((col_ + d) % n + n) % n;
    memorize();
}

void Keyboard::moveV(int d) {
    const int n = (int)rows_.size();
    row_ = ((row_ + d) % n + n) % n;
    const auto& r = rows_[(size_t)row_];
    int best = 0;
    float bd = 1e9f;
    for (int c = 0; c < (int)r.size(); c++) {
        const float dd = std::fabs(centerOf(row_, c) - memX_);
        if (dd < bd) {
            bd = dd;
            best = c;
        }
    }
    col_ = best;  // memX_ se conserva: al volver a la fila anterior se vuelve a la misma zona
}

void Keyboard::insert(const std::string& s) {
    if (length() >= maxLen_) return;
    text_.insert(cur_, s);
    cur_ += s.size();
}

void Keyboard::backspace() {
    if (cur_ == 0) return;
    size_t i = cur_ - 1;
    while (i > 0 && (((unsigned char)text_[i]) & 0xC0) == 0x80) i--;
    text_.erase(i, cur_ - i);
    cur_ = i;
}

void Keyboard::cursorLeft() {
    if (cur_ == 0) return;
    size_t i = cur_ - 1;
    while (i > 0 && (((unsigned char)text_[i]) & 0xC0) == 0x80) i--;
    cur_ = i;
}

void Keyboard::cursorRight() {
    if (cur_ >= text_.size()) return;
    cur_ += charLen(text_, cur_);
}

Result Keyboard::press(const Key& k) {
    switch (k.kind) {
        case Kind::Char:
            insert(shift_ ? upper(k.out) : k.out);
            shift_ = false;
            break;
        case Kind::Shift: shift_ = !shift_; break;
        case Kind::Symbols:
            page_ = 1 - page_;
            rebuild();
            clampSel();
            break;
        case Kind::Space: insert(" "); break;
        case Kind::Back: backspace(); break;
        case Kind::Cancel:
            open_ = false;
            return Result::Cancelled;
        case Kind::Accept:
            open_ = false;
            return Result::Accepted;
    }
    return Result::None;
}

Result Keyboard::update(const Input& in) {
    if (!open_ || rows_.empty()) return Result::None;
    if (in.minus) {
        open_ = false;
        return Result::Cancelled;
    }
    if (in.plus) {
        open_ = false;
        return Result::Accepted;
    }
    if (in.tap) {  // pantalla táctil: tocar una tecla la selecciona y la pulsa
        for (const Box& b : layout()) {
            if (in.tapX >= b.x && in.tapX < b.x + b.w && in.tapY >= b.y && in.tapY < b.y + b.h) {
                row_ = b.row;
                col_ = b.col;
                memorize();
                return press(rows_[(size_t)row_][(size_t)col_]);
            }
        }
    }
    if (in.left) moveH(-1);
    if (in.right) moveH(1);
    if (in.up) moveV(-1);
    if (in.down) moveV(1);
    if (in.l) cursorLeft();
    if (in.r) cursorRight();
    if (in.zl) cur_ = 0;
    if (in.zr) cur_ = text_.size();
    if (in.bRep) backspace();
    if (in.x) insert(" ");
    if (in.y && !numeric_) shift_ = !shift_;
    if (in.a) return press(rows_[(size_t)row_][(size_t)col_]);
    return Result::None;
}

}  // namespace osk
