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
    int64_t rootCatId = 0, subCatId = 0;  // IDs de categoría (0 = desconocido)
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
    int scanned = 0;  // búsqueda local: cuántos mods se revisaron (0 = búsqueda del servidor)
};

// Categoría de GameBanana (p. ej. "Skins" > "Mario").
struct Category {
    int64_t id = 0;
    std::string name;
    int64_t count = -1;  // mods que contiene (-1 = no se sabe)
    int64_t parent = 0;  // 0 = categoría raíz
};

struct CategoryList {
    bool ok = false;
    bool stale = false;    // viene de la caché del disco porque no hubo red (puede estar desactualizada)
    bool learned = false;  // lista parcial reconstruida con los mods ya vistos (la API de categorías no respondió)
    std::string err;
    std::vector<Category> cats;
};

enum class Sort { Default, New, Updated };
const char* sortKey(Sort s);

void setGameId(int id);
void clearFeedCache();  // olvida las páginas del plan B de la búsqueda (los tests lo usan)
int gameId();

Page browse(int page, Sort sort, int perPage);
Page search(const std::string& query, int page, int perPage);

// --- Filtros por categoría
// Hijos de una categoría (parent 0 = categorías raíz del juego). Usa caché (memoria + disco, 24 h) y, si la API
// de categorías no responde, las que se han ido viendo en los mods ya descargados.
CategoryList categories(int64_t parent);
void loadCategoryCache();  // lee categories.json (llamar al arrancar)
void clearCategoryCache();
// Mods de una categoría (incluye sus subcategorías). catId 0 = sin filtro.
Page browseCategory(int64_t catId, int page, Sort sort, int perPage);
// Búsqueda de texto dentro de una categoría (el servidor no combina ambas cosas: se filtra en local).
Page searchInCategory(int64_t catId, const std::string& query);
// Rellena out con la ficha completa. base aporta modelo/id y lo que ya se sepa.
bool detail(const Item& base, Detail& out, std::string& err);
// ID del archivo más reciente de un mod (para detectar actualizaciones). 0 si falla.
int64_t latestFileId(const std::string& model, int64_t id);

// Expuestos para los tests
bool parseRecords(const json::Value& root, Page& page);
bool parseCategories(const json::Value& root, std::vector<Category>& out, bool* complete = nullptr);
int64_t catIdFromUrl(const std::string& url);  // "https://gamebanana.com/mods/cats/123" -> 123
bool parseDetail(const json::Value& root, Detail& out);
void parseFiles(const json::Value& files, std::vector<File>& out);

}  // namespace gb
