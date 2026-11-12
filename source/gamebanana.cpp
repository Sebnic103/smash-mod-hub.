#include "gamebanana.hpp"

#include <algorithm>

#include "http.hpp"
#include "util.hpp"

namespace gb {

static int g_gameId = 6498;
static const char* kApi = "https://gamebanana.com/apiv11";

void setGameId(int id) { g_gameId = id > 0 ? id : 6498; }
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
    if (r["_aRootCategory"].has("_sName")) it.rootCat = r["_aRootCategory"]["_sName"].str(it.rootCat);
    else if (r["_aSuperCategory"].has("_sName")) it.rootCat = r["_aSuperCategory"]["_sName"].str(it.rootCat);
    if (r["_aSubCategory"].has("_sName")) it.subCat = r["_aSubCategory"]["_sName"].str(it.subCat);
    else if (r["_aCategory"].has("_sName")) it.subCat = r["_aCategory"]["_sName"].str(it.subCat);
    parseImages(r["_aPreviewMedia"], it.images);
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
static Page fetchPage(const std::string& url, int page) {
    Page p;
    p.page = page;
    std::string body;
    http::Result r = http::get(url, body);
    if (!r.ok()) {
        p.err = r.describe();
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

static std::string lowerContains(const std::string& hay) { return util::lower(hay); }

Page search(const std::string& query, int page, int perPage) {
    std::string url = std::string(kApi) + "/Util/Search/Results?_sSearchString=" + util::urlEncode(query) + "&_idGameRow=" + std::to_string(g_gameId) + "&_sModelName=Mod&_nPage=" + std::to_string(page) + "&_nPerpage=" + std::to_string(perPage);
    Page p = fetchPage(url, page);
    if (p.ok && !p.items.empty()) return p;
    // Plan B: si el buscador no responde como se espera, filtramos las primeras páginas del feed del juego.
    util::log("search: plan B (filtrado local) para '%s' (%s)", query.c_str(), p.ok ? "sin resultados" : p.err.c_str());
    Page out;
    out.page = 1;
    std::string q = util::lower(query);
    bool anyOk = false;
    for (int pg = 1; pg <= 6; pg++) {
        Page f = browse(pg, Sort::Default, 50);
        if (!f.ok) {
            if (!anyOk) out.err = f.err;
            break;
        }
        anyOk = true;
        for (auto& it : f.items) {
            if (lowerContains(it.name).find(q) != std::string::npos || lowerContains(it.author).find(q) != std::string::npos || lowerContains(it.category()).find(q) != std::string::npos) out.items.push_back(it);
        }
        if (f.complete || f.items.empty()) break;
    }
    out.ok = anyOk;
    out.complete = true;
    out.total = (int64_t)out.items.size();
    return out;
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
