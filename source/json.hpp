#pragma once
// Parser/serializador JSON mínimo (sin dependencias).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;

    static Value S(const std::string& s) {
        Value v;
        v.type_ = Type::String;
        v.s_ = s;
        return v;
    }
    static Value N(double n) {
        Value v;
        v.type_ = Type::Number;
        v.n_ = n;
        return v;
    }
    static Value B(bool b) {
        Value v;
        v.type_ = Type::Bool;
        v.b_ = b;
        return v;
    }
    static Value Obj() {
        Value v;
        v.type_ = Type::Object;
        return v;
    }
    static Value Arr() {
        Value v;
        v.type_ = Type::Array;
        return v;
    }
    static const Value& null();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Number; }

    // Acceso seguro: devuelve null() si la clave/índice no existe o el tipo no coincide.
    const Value& operator[](const std::string& key) const;
    const Value& at(size_t i) const;
    bool has(const std::string& key) const;
    size_t size() const;

    std::string str(const std::string& def = "") const { return type_ == Type::String ? s_ : def; }
    double num(double def = 0) const { return type_ == Type::Number ? n_ : def; }
    int64_t i64(int64_t def = 0) const { return type_ == Type::Number ? (int64_t)n_ : def; }
    bool boolean(bool def = false) const { return type_ == Type::Bool ? b_ : def; }

    const std::vector<Value>& elems() const { return a_; }
    const std::vector<std::pair<std::string, Value>>& items() const { return o_; }

    // Construcción
    Value& set(const std::string& key, Value v);
    Value& push(Value v);

    std::string dump(int indent = -1) const;

private:
    void dumpTo(std::string& out, int indent, int level) const;

    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> a_;
    std::vector<std::pair<std::string, Value>> o_;
};

bool parse(const std::string& text, Value& out, std::string* err = nullptr);

}  // namespace json
