#pragma once
#include <cstddef>
#include <string>

#include "common.hpp"

namespace md5 {
// MD5 incremental: se alimenta con trozos en orden (p. ej. mientras se descarga) y se pide el resultado al final.
class Hasher {
public:
    Hasher();
    ~Hasher();
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    void update(const void* data, size_t n);
    std::string finish();  // en minúsculas; solo se puede llamar una vez

private:
    struct Impl;
    Impl* p_;
};

std::string hex(const std::string& data);
// MD5 de un archivo (en minúsculas). Vacío si no se pudo leer o se canceló.
std::string file(const std::string& path, Progress* progress = nullptr);
}  // namespace md5
