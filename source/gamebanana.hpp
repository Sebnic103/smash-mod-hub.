#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

namespace gb {

struct Image {
    std::string base, file, f100, f220, f530;
};

struct Item {
    int64_t id = 0;
    std::string model = "Mod";  // "Mod" o "Sound"
    std::string name, author, rootCat, subCat, version, profileUrl;
    int64_t added = 0, modified = 0, updated = 0;
    int64_t likes = -1, views = -1;
    std::vector<Image> images;

    std::string thumbUrl() const;                // miniatura para la cuadrícula
    std::string galleryUrl(size_t index) const;  // imagen para el detalle
    std::string storeThumbUrl() const;           // la que se guarda junto al mod instalado
    std::string category() const;
};

struct File {
    int64_t id = 0;
    std::string name, md5, desc, url;
    uint64_t size = 0;
    int64_t added = 0;
};

struct Detail {
    Item item;
    std::string text;  // descripción en texto plano
    std::vector<File> files;
    int64_t downloads = -1;
};

struct Page {
    bool ok = false;
    std::string err;
    std::vector<Item> items;
    int64_t total = 0;
    bool complete = false;  // true si es la última página
    int page = 1;
    int dropped = 0;  // registros descartados por no ser de este juego / tipo
};

enum class Sort { Default, New, Updated };
const char* sortKey(Sort s);

void setGameId(int id);
int gameId();

Page browse(int page, Sort sort, int perPage);
Page search(const std::string& query, int page, int perPage);
// Rellena out con la ficha completa. base aporta modelo/id y lo que ya se sepa.
bool detail(const Item& base, Detail& out, std::string& err);
// ID del archivo más reciente de un mod (para detectar actualizaciones). 0 si falla.
int64_t latestFileId(const std::string& model, int64_t id);

// Expuestos para los tests
bool parseRecords(const json::Value& root, Page& page);
bool parseDetail(const json::Value& root, Detail& out);
void parseFiles(const json::Value& files, std::vector<File>& out);

}  // namespace gb
