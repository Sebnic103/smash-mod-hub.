#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

const Value& Value::null() {
    static const Value v;
    return v;
}

const Value& Value::operator[](const std::string& key) const {
    if (type_ != Type::Object) return null();
    for (const auto& kv : o_)
        if (kv.first == key) return kv.second;
    return null();
}

const Value& Value::at(size_t i) const {
    if (type_ != Type::Array || i >= a_.size()) return null();
    return a_[i];
}

bool Value::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& kv : o_)
        if (kv.first == key) return true;
    return false;
}

size_t Value::size() const {
    if (type_ == Type::Array) return a_.size();
    if (type_ == Type::Object) return o_.size();
    return 0;
}

Value& Value::set(const std::string& key, Value v) {
    if (type_ != Type::Object) {
        *this = Obj();
    }
    for (auto& kv : o_) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return *this;
        }
    }
    o_.emplace_back(key, std::move(v));
    return *this;
}

Value& Value::push(Value v) {
    if (type_ != Type::Array) {
        *this = Arr();
    }
    a_.push_back(std::move(v));
    return *this;
}

// ------------------------------------------------------------------ dump
static void dumpStr(const std::string& s, std::string& o) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    o += '"';
}

void Value::dumpTo(std::string& o, int indent, int level) const {
    auto nl = [&](int lv) {
        if (indent < 0) return;
        o += '\n';
        o.append((size_t)(indent * lv), ' ');
    };
    switch (type_) {
        case Type::Null: o += "null"; break;
        case Type::Bool: o += b_ ? "true" : "false"; break;
        case Type::Number: {
            char b[40];
            if (std::floor(n_) == n_ && std::fabs(n_) < 1e15)
                snprintf(b, sizeof b, "%lld", (long long)n_);
            else
                snprintf(b, sizeof b, "%.17g", n_);
            o += b;
            break;
        }
        case Type::String: dumpStr(s_, o); break;
        case Type::Array:
            o += '[';
            for (size_t i = 0; i < a_.size(); i++) {
                if (i) o += ',';
                nl(level + 1);
                a_[i].dumpTo(o, indent, level + 1);
            }
            if (!a_.empty()) nl(level);
            o += ']';
            break;
        case Type::Object:
            o += '{';
            for (size_t i = 0; i < o_.size(); i++) {
                if (i) o += ',';
                nl(level + 1);
                dumpStr(o_[i].first, o);
                o += indent < 0 ? ":" : ": ";
                o_[i].second.dumpTo(o, indent, level + 1);
            }
            if (!o_.empty()) nl(level);
            o += '}';
            break;
    }
}

std::string Value::dump(int indent) const {
    std::string o;
    dumpTo(o, indent, 0);
    return o;
}

// ------------------------------------------------------------------ parse
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    std::string err;

    explicit Parser(const std::string& str) : s(str) {}

    bool fail(const char* m) {
        if (err.empty()) err = std::string(m) + " en posición " + std::to_string(i);
        return false;
    }

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    }

    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) {
            o += (char)cp;
        } else if (cp < 0x800) {
            o += (char)(0xC0 | (cp >> 6));
            o += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += (char)(0xE0 | (cp >> 12));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xF0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& out) {
        if (i + 4 > s.size()) return fail("\\u incompleto");
        unsigned v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return fail("hex inválido");
        }
        i += 4;
        out = v;
        return true;
    }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("se esperaba \"");
        i++;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return fail("escape incompleto");
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        size_t save = i;
                        i += 2;
                        unsigned lo;
                        if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            i = save;
                            err.clear();
                        }
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fail("escape inválido");
            }
        }
        return fail("cadena sin cerrar");
    }

    bool parseNumber(Value& out) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') i++;
        if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) {
            i = start;
            return fail("valor inesperado");
        }
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) i++;
        out = Value::N(strtod(s.substr(start, i - start).c_str(), nullptr));
        return true;
    }

    bool parseValue(Value& out) {
        ws();
        if (i >= s.size()) return fail("fin inesperado");
        char c = s[i];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') {
            std::string str;
            if (!parseString(str)) return false;
            out = Value::S(str);
            return true;
        }
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            out = Value::B(true);
            return true;
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            out = Value::B(false);
            return true;
        }
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            out = Value();
            return true;
        }
        return parseNumber(out);
    }

    bool parseArray(Value& out) {
        if (++depth > 64) return fail("anidado demasiado profundo");
        i++;  // [
        out = Value::Arr();
        ws();
        if (i < s.size() && s[i] == ']') {
            i++;
            depth--;
            return true;
        }
        while (true) {
            Value v;
            if (!parseValue(v)) return false;
            out.push(std::move(v));
            ws();
            if (i >= s.size()) return fail("array sin cerrar");
            if (s[i] == ',') {
                i++;
                continue;
            }
            if (s[i] == ']') {
                i++;
                depth--;
                return true;
            }
            return fail("se esperaba , o ]");
        }
    }

    bool parseObject(Value& out) {
        if (++depth > 64) return fail("anidado demasiado profundo");
        i++;  // {
        out = Value::Obj();
        ws();
        if (i < s.size() && s[i] == '}') {
            i++;
            depth--;
            return true;
        }
        while (true) {
            ws();
            std::string key;
            if (!parseString(key)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') return fail("se esperaba :");
            i++;
            Value v;
            if (!parseValue(v)) return false;
            out.set(key, std::move(v));
            ws();
            if (i >= s.size()) return fail("objeto sin cerrar");
            if (s[i] == ',') {
                i++;
                continue;
            }
            if (s[i] == '}') {
                i++;
                depth--;
                return true;
            }
            return fail("se esperaba , o }");
        }
    }
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string* err) {
    Parser p(text);
    Value v;
    if (!p.parseValue(v)) {
        if (err) *err = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        if (err) *err = "contenido extra tras el JSON";
        return false;
    }
    out = std::move(v);
    return true;
}

}  // namespace json
