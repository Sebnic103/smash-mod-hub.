#pragma once
#include <string>

#include "common.hpp"

namespace md5 {
std::string hex(const std::string& data);
// MD5 de un archivo (en minúsculas). Vacío si no se pudo leer o se canceló.
std::string file(const std::string& path, Progress* progress = nullptr);
}  // namespace md5
