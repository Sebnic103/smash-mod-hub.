#include "gamebanana.hpp"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>
#include <set>

#include "http.hpp"
#include "sys.hpp"
#include "util.hpp"

namespace gb {

static int g_gameId = 6498;
static const char* kApi = "https://gamebanana.com/apiv11";

// Páginas del feed que usa el plan B de la búsqueda (y la búsqueda dentro de una categoría): se guardan 10 minutos
// para que buscar varias veces no vuelva a bajar y analizar varias páginas de JSON cada vez.
// Clave: ID de categoría (0 = el feed general del juego).
struct FeedCache {
    std::vector<Page> pages;
    uint64_t at = 0;
};
static sys::Mutex g_feedMx;
static std::map<int64_t, FeedCache> g_feeds;
static const uint64_t kFeedTtlMs = 10 * 60 * 1000;

static void clearCategoryMemory();

void clearFeedCache() {
    sys::Lock l(g_feedMx);
    g_feeds.clear();
}

void setGameId(int id) {
    g_gameId = id > 0 ? id : 6498;
    clearFeedCache();
    clearCategoryMemory();
}
int gameId() { return g_gameId; }

const char* sortKey(Sort s) {
    switch (s) {
        case Sort::New: return "new";
        case Sort::Updated: return "updated";
        default: return "default";
    }
}

// ------------------------------------------------------------ Item
static std::string joinImg(const Image& i, const std::string& f) { return (i.base.empty() || f.empty()) ? std::string() : i.base + "/" + f; }

std::string Item::thumbUrl() const {
    if (images.empty()) return "";
    const Image& i = images[0];
    for (const std::string* f : {&i.f220, &i.f100, &i.file})
        if (!f->empty()) return joinImg(i, *f);
    return "";
}

std::string Item::galleryUrl(size_t idx) const {
    if (idx >= images.size()) return "";
    const Image& i = images[idx];
    for (const std::string* f : {&i.f530, &i.file, &i.f220, &i.f100})
        if (!f->empty()) return joinImg(i, *f);
    return "";
}

std::string Item::storeThumbUrl() const {
    if (images.empty()) return "";
    const Image& i = images[0];
    for (const std::string* f : {&i.f530, &i.f220, &i.file, &i.f100})
        if (!f->empty()) return joinImg(i, *f);
    return "";
}

std::string Item::category() const {
    if (!rootCat.empty() && !subCat.empty() && rootCat != subCat) return rootCat + " / " + subCat;
    return !rootCat.empty() ? rootCat : subCat;
}

// ------------------------------------------------------------ parseo
static void parseImages(const json::Value& pm, std::vector<Image>& out) {
    const json::Value& arr = pm["_aImages"];
    if (!arr.isArray()) return;
    std::vector<Image> imgs;
    for (const auto& e : arr.elems()) {
        Image im;
        im.base = e["_sBaseUrl"].str();
        im.file = e["_sFile"].str();
        im.f100 = e["_sFile100"].str();
        im.f220 = e["_sFile220"].str();
        im.f530 = e["_sFile530"].str();
        if (!im.base.empty() && !im.file.empty()) imgs.push_back(im);
    }
    if (!imgs.empty()) out = imgs;
}

int64_t catIdFromUrl(const std::string& url) {
    size_t p = url.find("cats/");
    if (p == std::string::npos) return 0;
    p += 5;
    int64_t v = 0;
    int digits = 0;
    while (p < url.size() && url[p] >= '0' && url[p] <= '9' && digits < 15) {
        v = v * 10 + (url[p] - '0');
        p++;
        digits++;
    }
    return v;
}

static void fillCat(const json::Value& o, std::string& name, int64_t& id) {
    name = o["_sName"].str(name);
    int64_t v = o["_idRow"].i64(0);
    if (v <= 0) v = catIdFromUrl(o["_sProfileUrl"].str());
    if (v > 0) id = v;
}

static void fillItem(const json::Value& r, Item& it) {
    if (r.has("_idRow")) it.id = r["_idRow"].i64(it.id);
    if (r.has("_sModelName")) it.model = r["_sModelName"].str(it.model);
    if (r.has("_sName")) it.name = r["_sName"].str(it.name);
    if (r.has("_sProfileUrl")) it.profileUrl = r["_sProfileUrl"].str(it.profileUrl);
    if (r.has("_sVersion") && !r["_sVersion"].str().empty()) it.version = r["_sVersion"].str();
    if (r.has("_tsDateAdded")) it.added = r["_tsDateAdded"].i64(it.added);
    if (r.has("_tsDateModified")) it.modified = r["_tsDateModified"].i64(it.modified);
    if (r.has("_tsDateUpdated")) it.updated = r["_tsDateUpdated"].i64(it.updated);
    if (r.has("_nLikeCount")) it.likes = r["_nLikeCount"].i64(it.likes);
    if (r.has("_nViewCount")) it.views = r["_nViewCount"].i64(it.views);
    if (r["_aSubmitter"].has("_sName")) it.author = r["_aSubmitter"]["_sName"].str(it.author);
    if (r["_aRootCategory"].has("_sName")) fillCat(r["_aRootCategory"], it.rootCat, it.rootCatId);
    else if (r["_aSuperCategory"].has("_sName")) fillCat(r["_aSuperCategory"], it.rootCat, it.rootCatId);
    if (r["_aSubCategory"].has("_sName")) fillCat(r["_aSubCategory"], it.subCat, it.subCatId);
    else if (r["_aCategory"].has("_sName")) fillCat(r["_aCategory"], it.subCat, it.subCatId);
    parseImages(r["_aPreviewMedia"], it.images);
}

// ------------------------------------------------------------ categorías (estado)
static sys::Mutex g_catMx;
struct CatEntry {
    std::vector<Category> cats;
    int64_t savedAt = 0;  // segundos Unix
};
static std::map<int64_t, CatEntry> g_catCache;  // padre -> hijos
static std::map<int64_t, Category> g_learned;   // categorías vistas en los mods ya descargados
static const int64_t kCatTtlSec = 24 * 3600;
static std::atomic<int> g_catVariant[2] = {{0}, {0}};  // variante de URL que funcionó (raíz / hijos)

static void clearCategoryMemory() {
    sys::Lock l(g_catMx);
    g_catCache.clear();
    g_learned.clear();
}

static void learnCategories(const Item& it) {
    if (it.rootCatId <= 0 && it.subCatId <= 0) return;
    sys::Lock l(g_catMx);
    auto put = [&](int64_t id, const std::string& name, int64_t parent) {
        if (id <= 0 || name.empty()) return;
        if (g_learned.size() > 2000) return;  // tope: la memoria no crece sin límite
        Category& c = g_learned[id];
        c.id = id;
        c.name = name;
        if (parent > 0 || c.parent == 0) c.parent = parent;
    };
    put(it.rootCatId, it.rootCat, 0);
    if (it.subCatId != it.rootCatId) put(it.subCatId, it.subCat, it.rootCatId);
}

bool parseRecords(const json::Value& root, Page& page) {
    const json::Value& recs = root["_aRecords"];
    if (!recs.isArray()) return false;
    page.total = root["_aMetadata"]["_nRecordCount"].i64(0);
    page.complete = root["_aMetadata"]["_bIsComplete"].boolean(false);
    for (const auto& r : recs.elems()) {
        std::string model = r["_sModelName"].str("Mod");
        if (model != "Mod" && model != "Sound") {
            page.dropped++;
            continue;
        }
        int64_t gid = r["_aGame"]["_idRow"].i64(0);
        if (g_gameId && gid && gid != g_gameId) {  // por si el filtro del servidor no se aplica
            page.dropped++;
            continue;
        }
        Item it;
        fillItem(r, it);
        if (it.id == 0 || it.name.empty()) continue;
        learnCategories(it);
        page.items.push_back(std::move(it));
    }
    return true;
}

void parseFiles(const json::Value& files, std::vector<File>& out) {
    auto one = [&](const json::Value& e) {
        File f;
        f.id = e["_idRow"].i64();
        f.name = e["_sFile"].str();
        f.size = (uint64_t)e["_nFilesize"].num();
        f.md5 = util::lower(e["_sMd5Checksum"].str());
        f.desc = e["_sDescription"].str();
        f.added = e["_tsDateAdded"].i64();
        f.url = e["_sDownloadUrl"].str();
        if (f.url.empty() && f.id) f.url = "https://gamebanana.com/dl/" + std::to_string(f.id);
        if (f.id && !f.name.empty()) out.push_back(f);
    };
    if (files.isArray()) {
        for (const auto& e : files.elems()) one(e);
    } else if (files.isObject()) {  // formato antiguo: objeto indexado por ID
        for (const auto& kv : files.items()) one(kv.second);
    }
}

bool parseDetail(const json::Value& root, Detail& out) {
    if (!root.isObject()) return false;
    bool any = false;
    if (root.has("_sName") || root.has("_aFiles")) any = true;
    fillItem(root, out.item);
    if (root.has("_sText")) out.text = util::stripHtml(root["_sText"].str());
    else if (root.has("_sDescription")) out.text = util::stripHtml(root["_sDescription"].str());
    if (out.text.size() > 4000) out.text = util::clipUtf8(out.text, 4000) + "...";
    if (root.has("_nDownloadCount")) out.downloads = root["_nDownloadCount"].i64(-1);
    if (root.has("_aFiles")) {
        out.files.clear();
        parseFiles(root["_aFiles"], out.files);
        std::sort(out.files.begin(), out.files.end(), [](const File& a, const File& b) { return a.id > b.id; });
    }
    return any;
}

// ------------------------------------------------------------ red
// rejected (opcional): true si el servidor contestó pero NO aceptó la petición (HTTP 4xx o JSON de error), lo que
// indica una variante de URL no válida; false en fallos de red (no tiene sentido probar otra variante).
static Page fetchPage(const std::string& url, int page, bool* rejected = nullptr) {
    Page p;
    p.page = page;
    if (rejected) *rejected = false;
    std::string body;
    http::Result r = http::get(url, body);
    if (!r.ok()) {
        p.err = r.describe();
        if (rejected) *rejected = r.error.empty() && r.status >= 400 && r.status < 500;
        return p;
    }
    json::Value v;
    std::string perr;
    if (!json::parse(body, v, &perr)) {
        p.err = "JSON inválido: " + perr;
        util::log("fetchPage: JSON inválido (%s) %.200s", perr.c_str(), body.c_str());
        return p;
    }
    if (!parseRecords(v, p)) {
        p.err = "respuesta inesperada de GameBanana";
        if (rejected) *rejected = true;
        util::log("fetchPage: sin _aRecords %.200s", body.c_str());
        return p;
    }
    p.page = page;
    p.ok = true;
    if (p.items.empty() && p.dropped > 0) util::log("fetchPage: %d registros descartados (¿ID de juego %d correcto?)", p.dropped, g_gameId);
    return p;
}

Page browse(int page, Sort sort, int perPage) {
    std::string url = std::string(kApi) + "/Game/" + std::to_string(g_gameId) + "/Subfeed?_nPage=" + std::to_string(page) + "&_nPerpage=" + std::to_string(perPage) + "&_sSort=" + sortKey(sort);
    return fetchPage(url, page);
}

static Page feedPage(int64_t catId, int pg) {
    const uint64_t now = sys::nowMs();
    {
        sys::Lock l(g_feedMx);
        auto it = g_feeds.find(catId);
        if (it != g_feeds.end() && it->second.at && now - it->second.at < kFeedTtlMs && (size_t)pg <= it->second.pages.size()) return it->second.pages[(size_t)pg - 1];
    }
    Page f = catId > 0 ? browseCategory(catId, pg, Sort::Default, 50) : browse(pg, Sort::Default, 50);
    if (f.ok) {
        sys::Lock l(g_feedMx);
        if (g_feeds.size() >= 4 && g_feeds.find(catId) == g_feeds.end()) {  // pocas categorías en memoria: la más antigua fuera
            auto oldest = g_feeds.begin();
            for (auto i = g_feeds.begin(); i != g_feeds.end(); ++i)
                if (i->second.at < oldest->second.at) oldest = i;
            g_feeds.erase(oldest);
        }
        FeedCache& fc = g_feeds[catId];
        if (!fc.at || now - fc.at >= kFeedTtlMs) {
            fc.pages.clear();
            fc.at = now;
        }
        if ((size_t)pg == fc.pages.size() + 1) fc.pages.push_back(f);
    }
    return f;
}

// Búsquedas largas (plan B): cancelación y progreso para la interfaz. Sin esto, escanear varias páginas parecía un
// cuelgue (no había nada que ver) y una búsqueda nueva tenía que esperar a que acabara la anterior.
static std::atomic<int> g_searchGen{0}, g_scanDone{0}, g_scanTotal{0};

void cancelSearch() { g_searchGen++; }

void searchProgress(int& done, int& total) {
    done = g_scanDone.load();
    total = g_scanTotal.load();
}

// Filtra en local las primeras páginas del feed (de una categoría o del juego entero). Se detiene si se pide otra
// cosa (cancelSearch) y, pasado deadlineMs, devuelve lo que lleve encontrado en vez de seguir escaneando.
static Page localSearch(int64_t catId, const std::string& query, int maxPages, uint64_t deadlineMs) {
    Page out;
    out.page = 1;
    const std::string q = util::lower(query);
    const int gen = g_searchGen.load();
    const uint64_t t0 = sys::nowMs();
    g_scanDone = 0;
    g_scanTotal = maxPages;
    bool anyOk = false;
    for (int pg = 1; pg <= maxPages; pg++) {
        if (g_searchGen.load() != gen) {
            out.err = "cancelada";
            out.ok = false;
            return out;
        }
        if (anyOk && deadlineMs && sys::nowMs() - t0 > deadlineMs) {
            util::log("search: límite de tiempo (%llu ms), se devuelve lo encontrado en %d páginas", (unsigned long long)(sys::nowMs() - t0), pg - 1);
            break;
        }
        Page f = feedPage(catId, pg);
        if (!f.ok) {
            if (!anyOk) out.err = f.err;
            break;
        }
        anyOk = true;
        g_scanDone = pg;
        out.scanned += (int)f.items.size();
        for (auto& it : f.items) {
            if (util::lower(it.name).find(q) != std::string::npos || util::lower(it.author).find(q) != std::string::npos || util::lower(it.category()).find(q) != std::string::npos) out.items.push_back(it);
        }
        if (f.complete || f.items.empty()) break;
    }
    if (g_searchGen.load() != gen) {
        out.err = "cancelada";
        out.ok = false;
        return out;
    }
    out.ok = anyOk;
    out.complete = true;
    out.total = (int64_t)out.items.size();
    return out;
}

Page search(const std::string& query, int page, int perPage) {
    g_scanDone = 0;
    g_scanTotal = 0;
    std::string url = std::string(kApi) + "/Util/Search/Results?_sSearchString=" + util::urlEncode(query) + "&_idGameRow=" + std::to_string(g_gameId) + "&_sModelName=Mod&_nPage=" + std::to_string(page) + "&_nPerpage=" + std::to_string(perPage);
    Page p = fetchPage(url, page);
    if (p.ok && !p.items.empty()) return p;
    if (page > 1) {  // no hay más páginas de resultados: no se repite la búsqueda local (devolvía la página 1 otra vez)
        if (p.ok) p.complete = true;
        return p;
    }
    // Plan B para la primera página. Si el buscador falló se escanean hasta 4 páginas del feed (máx. 20 s); si contestó
    // "sin resultados" se mira solo lo más reciente (2 páginas, máx. 8 s) por si el buscador se quedó corto.
    const bool failed = !p.ok;
    util::log("search: plan B (filtrado local) para '%s' (%s)", query.c_str(), failed ? p.err.c_str() : "sin resultados");
    return localSearch(0, query, failed ? 4 : 2, failed ? 20000 : 8000);
}

// ------------------------------------------------------------ filtros por categoría
// Orden en Mod/Index (distinto al del Subfeed). Si el servidor no acepta el valor se prueba la alternativa y,
// como último recurso, se pide sin orden. La variante que funciona se recuerda.
static std::atomic<int> g_newVariant{0}, g_updVariant{0};

Page browseCategory(int64_t catId, int page, Sort sort, int perPage) {
    if (catId <= 0) return browse(page, sort, perPage);
    const std::string base = std::string(kApi) + "/Mod/Index?_nPage=" + std::to_string(page) + "&_nPerpage=" + std::to_string(perPage) + "&_aFilters%5BGeneric_Category%5D=" + std::to_string(catId);
    if (sort == Sort::Default) return fetchPage(base, page);
    const bool upd = sort == Sort::Updated;
    static const char* const keys[2][2] = {{"Generic_Newest", nullptr}, {"Generic_LatestModified", "Generic_LatestUpdated"}};
    std::atomic<int>& var = upd ? g_updVariant : g_newVariant;
    const int nKeys = upd ? 2 : 1;
    for (int v = var.load(); v < nKeys; v++) {
        bool rejected = false;
        Page p = fetchPage(base + "&_sSort=" + keys[upd ? 1 : 0][v], page, &rejected);
        if (p.ok) {
            var = v;
            return p;
        }
        if (!rejected) return p;  // fallo de red: otra variante no lo arregla
        util::log("browseCategory: el servidor no acepta _sSort=%s", keys[upd ? 1 : 0][v]);
    }
    var = nKeys;  // ninguna sirve: a partir de ahora sin orden
    return fetchPage(base, page);
}

Page searchInCategory(int64_t catId, const std::string& query) { return localSearch(catId, query, 8, 25000); }

// --- listado de categorías
bool parseCategories(const json::Value& root, std::vector<Category>& out, bool* complete) {
    const json::Value* recs = nullptr;
    for (const char* k : {"_aRecords", "_aModRootCategories", "_aSubCategories", "_aChildCategories"}) {
        if (root[k].isArray()) {
            recs = &root[k];
            break;
        }
    }
    if (!recs && root.isArray()) recs = &root;
    if (!recs) return false;
    if (complete) *complete = root["_aMetadata"].has("_bIsComplete") ? root["_aMetadata"]["_bIsComplete"].boolean(true) : true;
    std::set<int64_t> seen;
    for (const auto& r : recs->elems()) {
        const std::string model = r["_sModelName"].str();
        if (!model.empty() && model.find("Categ") == std::string::npos) return false;  // son mods, no categorías
        const int64_t gid = r["_aGame"]["_idRow"].i64(0);
        if (g_gameId && gid && gid != g_gameId) continue;
        Category c;
        c.id = r["_idRow"].i64(0);
        if (c.id <= 0) c.id = catIdFromUrl(r["_sProfileUrl"].str());
        c.name = util::trim(r["_sName"].str());
        for (const char* k : {"_nItemCount", "_nSubmissionCount", "_nModCount", "_nRecordCount"}) {
            if (r.has(k)) {
                c.count = r[k].i64(-1);
                break;
            }
        }
        if (c.id <= 0 || c.name.empty() || !seen.insert(c.id).second) continue;
        out.push_back(c);
        if (out.size() >= 400) break;
    }
    return true;
}

static bool fetchCategoryLevel(int64_t parent, std::vector<Category>& out, std::string& err) {
    const std::string g = std::to_string(g_gameId);
    const std::string n = std::to_string(parent);
    std::vector<std::string> variants;
    if (parent == 0) {
        variants = {"/Mod/Categories?_idGameRow=" + g + "&_sSort=a_to_z", "/Mod/Categories?_idGameRow=" + g, "/Mod/Categories?_idCategoryRow=0&_idGameRow=" + g};
    } else {
        variants = {"/Mod/Categories?_idCategoryRow=" + n + "&_sSort=a_to_z", "/Mod/Categories?_idCategoryRow=" + n, "/Mod/Categories?_idCategoryRow=" + n + "&_idGameRow=" + g};
    }
    std::set<int64_t> rootIds;
    {
        sys::Lock l(g_catMx);
        auto it = g_catCache.find(0);
        if (it != g_catCache.end())
            for (const auto& c : it->second.cats) rootIds.insert(c.id);
    }
    std::atomic<int>& remembered = g_catVariant[parent == 0 ? 0 : 1];
    const int nv = (int)variants.size();
    const int first = std::min(std::max(remembered.load(), 0), nv - 1);
    for (int k = 0; k < nv; k++) {
        const int vi = (first + k) % nv;
        std::vector<Category> acc;
        bool bad = false, networkFail = false;
        for (int pg = 1; pg <= 8 && !bad; pg++) {
            const std::string url = std::string(kApi) + variants[(size_t)vi] + "&_nPage=" + std::to_string(pg) + "&_nPerpage=50";
            std::string body;
            http::Result r = http::get(url, body);
            if (!r.ok()) {
                err = r.describe();
                networkFail = !(r.error.empty() && r.status >= 400 && r.status < 500);
                bad = true;
                break;
            }
            json::Value v;
            std::string perr;
            std::vector<Category> tmp;
            bool complete = true;
            if (!json::parse(body, v, &perr) || !parseCategories(v, tmp, &complete)) {
                err = "respuesta inesperada de GameBanana";
                bad = true;
                break;
            }
            if (parent != 0 && !tmp.empty()) {  // si el servidor ignoró "_idCategoryRow" devolvería las raíces (o a sí misma)
                bool hasSelf = false, allRoot = true;
                for (const auto& c : tmp) {
                    hasSelf = hasSelf || c.id == parent;
                    allRoot = allRoot && rootIds.count(c.id) > 0;
                }
                if (hasSelf || (allRoot && !rootIds.empty() && rootIds.count(parent) == 0 && tmp.size() >= 3)) {
                    err = "la API devolvió otra lista";
                    bad = true;
                    break;
                }
            }
            acc.insert(acc.end(), tmp.begin(), tmp.end());
            if (complete || tmp.empty()) break;
        }
        if (networkFail) return false;  // sin red no tiene sentido seguir probando
        if (bad) continue;
        if (parent == 0 && acc.empty()) {
            err = "lista de categorías vacía";
            continue;
        }
        remembered = vi;
        out = std::move(acc);
        return true;
    }
    return false;
}

static std::string catCachePath() { return util::join(paths::cache(), "categories.json"); }

static void saveCategoryCache() {
    json::Value root = json::Value::Obj();
    root.set("game", json::Value::N(g_gameId));
    json::Value lists = json::Value::Obj();
    {
        sys::Lock l(g_catMx);
        for (const auto& kv : g_catCache) {
            json::Value e = json::Value::Obj();
            e.set("t", json::Value::N((double)kv.second.savedAt));
            json::Value arr = json::Value::Arr();
            for (const auto& c : kv.second.cats) {
                json::Value o = json::Value::Obj();
                o.set("i", json::Value::N((double)c.id));
                o.set("n", json::Value::S(c.name));
                o.set("c", json::Value::N((double)c.count));
                arr.push(o);
            }
            e.set("c", arr);
            lists.set(std::to_string(kv.first), e);
        }
    }
    root.set("lists", lists);
    util::writeFileAtomic(catCachePath(), root.dump());
}

void loadCategoryCache() {
    std::string text;
    json::Value v;
    if (!util::readFile(catCachePath(), text) || !json::parse(text, v)) return;
    if (v["game"].i64(0) != g_gameId) return;
    sys::Lock l(g_catMx);
    for (const auto& kv : v["lists"].items()) {
        int64_t parent = atoll(kv.first.c_str());
        CatEntry e;
        e.savedAt = kv.second["t"].i64(0);
        for (const auto& o : kv.second["c"].elems()) {
            Category c;
            c.id = o["i"].i64(0);
            c.name = o["n"].str();
            c.count = o["c"].i64(-1);
            c.parent = parent;
            if (c.id > 0 && !c.name.empty()) e.cats.push_back(c);
        }
        if (e.cats.size() > 0 || parent != 0) g_catCache[parent] = std::move(e);
    }
}

void clearCategoryCache() {
    clearCategoryMemory();
    util::rmrf(catCachePath());
}

CategoryList categories(int64_t parent) {
    CategoryList res;
    const int64_t nowS = (int64_t)time(nullptr);
    CatEntry stale;
    bool haveStale = false;
    {
        sys::Lock l(g_catMx);
        auto it = g_catCache.find(parent);
        if (it != g_catCache.end()) {
            const int64_t d = nowS - it->second.savedAt;
            if (d >= 0 && d < kCatTtlSec) {
                res.ok = true;
                res.cats = it->second.cats;
                return res;
            }
            stale = it->second;
            haveStale = true;
        }
    }
    std::vector<Category> list;
    std::string err;
    if (fetchCategoryLevel(parent, list, err)) {
        for (auto& c : list) c.parent = parent;
        {
            sys::Lock l(g_catMx);
            CatEntry& e = g_catCache[parent];
            e.cats = list;
            e.savedAt = nowS;
        }
        saveCategoryCache();
        res.ok = true;
        res.cats = std::move(list);
        return res;
    }
    util::log("categories(%lld): %s", (long long)parent, err.c_str());
    if (haveStale) {
        res.ok = true;
        res.stale = true;
        res.cats = stale.cats;
        return res;
    }
    {
        sys::Lock l(g_catMx);
        for (const auto& kv : g_learned)
            if (kv.second.parent == parent) res.cats.push_back(kv.second);
    }
    if (!res.cats.empty()) {
        std::sort(res.cats.begin(), res.cats.end(), [](const Category& a, const Category& b) { return util::lower(a.name) < util::lower(b.name); });
        res.ok = true;
        res.learned = true;
        return res;
    }
    res.err = err.empty() ? "sin respuesta" : err;
    return res;
}

bool detail(const Item& base, Detail& out, std::string& err) {
    out = Detail();
    out.item = base;
    const std::string m = base.model.empty() ? "Mod" : base.model;
    const std::string id = std::to_string(base.id);
    const std::string urls[] = {
        std::string(kApi) + "/" + m + "/" + id + "/ProfilePage",
        std::string(kApi) + "/" + m + "/" + id + "?_csvProperties=_sName,_sText,_aFiles,_aPreviewMedia,_aSubmitter,_nLikeCount,_nViewCount,_nDownloadCount,_tsDateAdded,_tsDateUpdated,_tsDateModified,_sVersion,_aCategory,_aSuperCategory",
        std::string(kApi) + "/" + m + "/" + id + "?_csvProperties=_aFiles",
    };
    bool parsedAny = false;
    for (const auto& u : urls) {
        std::string body;
        http::Result r = http::get(u, body);
        if (!r.ok()) {
            err = r.describe();
            continue;
        }
        json::Value v;
        std::string perr;
        if (!json::parse(body, v, &perr)) {
            err = "JSON inválido: " + perr;
            continue;
        }
        if (parseDetail(v, out)) {
            parsedAny = true;
            if (!out.files.empty()) break;
        }
    }
    if (parsedAny) err.clear();
    else if (err.empty()) err = "respuesta inesperada de GameBanana";
    return parsedAny;
}

int64_t latestFileId(const std::string& model, int64_t id) {
    std::string url = std::string(kApi) + "/" + (model.empty() ? "Mod" : model) + "/" + std::to_string(id) + "?_csvProperties=_aFiles";
    std::string body;
    if (!http::get(url, body).ok()) return 0;
    json::Value v;
    if (!json::parse(body, v)) return 0;
    std::vector<File> files;
    parseFiles(v["_aFiles"], files);
    int64_t best = 0;
    for (const auto& f : files) best = std::max(best, f.id);
    return best;
}

}  // namespace gb
