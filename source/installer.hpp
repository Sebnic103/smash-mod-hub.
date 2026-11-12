#pragma once
#include <string>
#include <vector>

#include "common.hpp"

struct InstallResult {
    bool ok = false;
    std::string err;
    std::vector<std::string> folders;  // carpetas finales creadas dentro de targetDir
};

// Normaliza la ruta de una entrada de un archivo comprimido a una ruta relativa segura
// (sin "..", sin raíz, sin caracteres no válidos en FAT32). false = descartar la entrada.
bool safeRelative(const char* pathname, std::string& out);

// Extrae un .zip/.7z/.rar/.tar.* con libarchive en destDir (protegido contra rutas "../").
// El progreso usa los bytes comprimidos leídos (now/total).
bool extractArchive(const std::string& archive, const std::string& destDir, Progress* progress, std::string& err);

// Busca las "raíces de mod" dentro de un directorio extraído: carpetas que contienen
// fighter/, ui/, sound/, stage/... (o config.json / info.toml / preview.webp).
std::vector<std::string> findModRoots(const std::string& dir);

// Mueve las raíces de mod encontradas a targetDir. Si replaceFolder no está vacío y hay una
// sola raíz, reemplaza esa carpeta (actualización). fallbackName se usa cuando la raíz es la
// propia carpeta extraída (zip sin carpeta contenedora).
InstallResult installFromDir(const std::string& extractedDir, const std::string& fallbackName, const std::string& targetDir, const std::string& replaceFolder);
