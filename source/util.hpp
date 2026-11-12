#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace paths {
// En la Switch la raíz es "sdmc:"; en los tests es una carpeta temporal.
void setRoot(const std::string& root);
std::string root();
std::string mods();          // <root>/ultimate/mods
std::string modsDisabled();  // <root>/ultimate/mods_disabled
std::string data();          // <root>/switch/smash-mod-hub
std::string tmp();
std::string downloads();
std::string thumbs();
std::string cache();
std::string importDir();  // archivos .zip/.7z/.rar que el usuario copia a mano para instalar
std::string dbFile();
std::string settingsFile();
std::string logFile();
// Rutas para el diagnóstico (Skyline / ARCropolis)
std::string smashTitleDir();  // <root>/atmosphere/contents/01006A800016E000
}  // namespace paths

namespace util {

std::string lower(std::string s);
bool startsWith(const std::string& s, const std::string& p);
bool endsWith(const std::string& s, const std::string& p);
std::string join(const std::string& a, const std::string& b);
std::string baseName(const std::string& p);
std::string dirName(const std::string& p);

bool exists(const std::string& p);
bool isDir(const std::string& p);
bool isFile(const std::string& p);
bool mkdirs(const std::string& p);
bool rmrf(const std::string& p);
uint64_t dirSize(const std::string& p);
bool copyFile(const std::string& from, const std::string& to);
bool moveDir(const std::string& from, const std::string& to);
bool readFile(const std::string& p, std::string& out);
bool writeFile(const std::string& p, const std::string& data);
bool writeFileAtomic(const std::string& p, const std::string& data);
std::vector<std::string> listDirs(const std::string& p);   // solo nombres, ordenados
std::vector<std::string> listFiles(const std::string& p);  // solo nombres, ordenados
uint64_t fileSize(const std::string& p);
uint64_t freeSpace(const std::string& p);

// Nombre seguro para FAT32 (sin <>:"/\|?*, sin puntos/espacios finales, sin punto inicial).
std::string sanitizeName(const std::string& s);
// Igual pero para un componente de una ruta dentro de un archivo (permite punto inicial).
std::string sanitizeComponent(const std::string& s);

std::string clipUtf8(const std::string& s, size_t maxBytes);  // recorta sin partir un carácter UTF-8
std::string humanSize(uint64_t bytes);
std::string stripHtml(const std::string& html);
std::string urlEncode(const std::string& s);
std::string fnvHex(const std::string& s);
std::string formatDate(int64_t unixTs);  // YYYY-MM-DD
std::string trim(const std::string& s);

void setLogPath(const std::string& p);
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace util
