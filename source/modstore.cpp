#include "modstore.hpp"

#include <algorithm>
#include <set>

#include "json.hpp"
#include "util.hpp"

// ------------------------------------------------------------------ Mod
std::string Mod::dir() const { return util::join(enabled ? paths::mods() : paths::modsDisabled(), folder); }

std::string Mod::imagePath() const {
    if (!meta.thumb.empty() && util::isFile(meta.thumb)) return meta.thumb;
    return localPreview;
}

// ------------------------------------------------------------------ info.toml
static void parseInfoToml(const std::string& path, Mod& m) {
    std::string text;
    if (!util::readFile(path, text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = util::trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = util::trim(line.substr(0, eq));
        std::string val = util::trim(line.substr(eq + 1));
        if (util::startsWith(val, "\"\"\"")) {  // descripciones multilínea: las ignoramos
            if (val.find("\"\"\"", 3) == std::string::npos) {
                size_t end = text.find("\"\"\"", pos);
                pos = end == std::string::npos ? text.size() : end + 3;
            }
            continue;
        }
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        if (key == "display_name") m.infoName = val;
        else if (key == "authors") m.infoAuthor = val;
        else if (key == "version") m.infoVersion = val;
        else if (key == "category") m.infoCategory = val;
    }
}

// ------------------------------------------------------------------ db
void ModStore::loadDb() {
    db_.clear();
    profiles_.clear();
    std::string text;
    if (!util::readFile(paths::dbFile(), text)) return;
    json::Value v;
    std::string err;
    if (!json::parse(text, v, &err)) {
        util::log("mods.json ilegible: %s", err.c_str());
        return;
    }
    for (const auto& kv : v["mods"].items()) {
        const json::Value& o = kv.second;
        Meta m;
        m.gbId = o["gb_id"].i64();
        m.model = o["model"].str();
        m.name = o["name"].str();
        m.author = o["author"].str();
        m.version = o["version"].str();
        m.category = o["category"].str();
        m.thumb = o["thumb"].str();
        m.installedAt = o["installed"].str();
        m.fileId = o["file_id"].i64();
        m.latestFileId = o["latest_file_id"].i64();
        db_[kv.first] = m;
    }
    for (const auto& kv : v["profiles"].items()) {
        std::vector<std::string> list;
        for (const auto& e : kv.second.elems()) list.push_back(e.str());
        profiles_[kv.first] = list;
    }
}

void ModStore::save() const {
    json::Value root = json::Value::Obj();
    root.set("version", json::Value::N(1));
    json::Value mods = json::Value::Obj();
    for (const auto& kv : db_) {
        const Meta& m = kv.second;
        json::Value o = json::Value::Obj();
        o.set("gb_id", json::Value::N((double)m.gbId));
        o.set("model", json::Value::S(m.model));
        o.set("name", json::Value::S(m.name));
        o.set("author", json::Value::S(m.author));
        o.set("version", json::Value::S(m.version));
        o.set("category", json::Value::S(m.category));
        o.set("thumb", json::Value::S(m.thumb));
        o.set("installed", json::Value::S(m.installedAt));
        o.set("file_id", json::Value::N((double)m.fileId));
        o.set("latest_file_id", json::Value::N((double)m.latestFileId));
        mods.set(kv.first, o);
    }
    root.set("mods", mods);
    json::Value profs = json::Value::Obj();
    for (const auto& kv : profiles_) {
        json::Value a = json::Value::Arr();
        for (const auto& s : kv.second) a.push(json::Value::S(s));
        profs.set(kv.first, a);
    }
    root.set("profiles", profs);
    if (!util::writeFileAtomic(paths::dbFile(), root.dump(1))) util::log("no se pudo guardar mods.json");
}

// ------------------------------------------------------------------ carga
void ModStore::load() {
    loadDb();
    mods_.clear();
    std::set<std::string> seen;
    auto scan = [&](const std::string& dir, bool enabled) {
        for (const auto& name : util::listDirs(dir)) {
            if (name.empty() || name[0] == '.') continue;
            if (seen.count(name)) {
                util::log("carpeta duplicada en activados/desactivados: %s (se ignora la copia de %s)", name.c_str(), dir.c_str());
                continue;
            }
            seen.insert(name);
            Mod m;
            m.folder = name;
            m.enabled = enabled;
            auto it = db_.find(name);
            if (it != db_.end()) m.meta = it->second;
            std::string full = util::join(dir, name);
            parseInfoToml(util::join(full, "info.toml"), m);
            for (const char* p : {"preview.webp", "preview.png", "preview.jpg", "preview.jpeg"}) {
                std::string pp = util::join(full, p);
                if (util::isFile(pp)) {
                    m.localPreview = pp;
                    break;
                }
            }
            auto sc = sizeCache_.find(name);
            if (sc != sizeCache_.end()) {
                m.size = sc->second;
                m.sizeKnown = true;
            }
            mods_.push_back(std::move(m));
        }
    };
    scan(paths::mods(), true);
    scan(paths::modsDisabled(), false);
    // Limpia entradas huérfanas del db
    for (auto it = db_.begin(); it != db_.end();) {
        if (!seen.count(it->first)) it = db_.erase(it);
        else ++it;
    }
    std::sort(mods_.begin(), mods_.end(), [](const Mod& a, const Mod& b) { return util::lower(a.title()) < util::lower(b.title()); });
}

int ModStore::findByFolder(const std::string& folder) const {
    for (size_t i = 0; i < mods_.size(); i++)
        if (mods_[i].folder == folder) return (int)i;
    return -1;
}

int ModStore::findByGb(int64_t gbId) const {
    if (!gbId) return -1;
    for (size_t i = 0; i < mods_.size(); i++)
        if (mods_[i].meta.gbId == gbId) return (int)i;
    return -1;
}

int ModStore::enabledCount() const {
    int n = 0;
    for (const auto& m : mods_) n += m.enabled ? 1 : 0;
    return n;
}

// ------------------------------------------------------------------ acciones
bool ModStore::setEnabled(size_t idx, bool enabled, std::string& err) {
    if (idx >= mods_.size()) return false;
    Mod& m = mods_[idx];
    if (m.enabled == enabled) return true;
    std::string from = m.dir();
    std::string toDir = enabled ? paths::mods() : paths::modsDisabled();
    std::string to = util::join(toDir, m.folder);
    if (util::exists(to)) {
        err = "ya existe " + to;
        return false;
    }
    util::mkdirs(toDir);
    if (!util::moveDir(from, to)) {
        err = "no se pudo mover " + m.folder;
        util::log("setEnabled: fallo moviendo %s -> %s", from.c_str(), to.c_str());
        return false;
    }
    m.enabled = enabled;
    return true;
}

int ModStore::setAll(bool enabled, std::string& err) {
    int changed = 0;
    for (size_t i = 0; i < mods_.size(); i++) {
        if (mods_[i].enabled == enabled) continue;
        std::string e;
        if (setEnabled(i, enabled, e)) changed++;
        else err = e;
    }
    return changed;
}

bool ModStore::remove(size_t idx, std::string& err) {
    if (idx >= mods_.size()) return false;
    Mod m = mods_[idx];
    if (!util::rmrf(m.dir())) {
        err = "no se pudo borrar " + m.folder;
        return false;
    }
    std::string thumb = m.meta.thumb;
    db_.erase(m.folder);
    sizeCache_.erase(m.folder);
    for (auto& p : profiles_) p.second.erase(std::remove(p.second.begin(), p.second.end(), m.folder), p.second.end());
    mods_.erase(mods_.begin() + (long)idx);
    if (!thumb.empty()) {  // borra la miniatura si ningún otro mod la usa
        bool used = false;
        for (const auto& kv : db_) used = used || kv.second.thumb == thumb;
        if (!used) util::rmrf(thumb);
    }
    save();
    return true;
}

void ModStore::recordInstall(const std::string& folder, const Meta& meta) {
    db_[folder] = meta;
    sizeCache_.erase(folder);
    save();
}

void ModStore::setLatest(int64_t gbId, int64_t latestFileId) {
    for (auto& kv : db_)
        if (kv.second.gbId == gbId) kv.second.latestFileId = latestFileId;
    for (auto& m : mods_)
        if (m.meta.gbId == gbId) m.meta.latestFileId = latestFileId;
}

void ModStore::setSize(const std::string& folder, uint64_t size) {
    sizeCache_[folder] = size;
    int i = findByFolder(folder);
    if (i >= 0) {
        mods_[(size_t)i].size = size;
        mods_[(size_t)i].sizeKnown = true;
    }
}

// ------------------------------------------------------------------ perfiles
std::vector<std::string> ModStore::profileNames() const {
    std::vector<std::string> v;
    for (const auto& kv : profiles_) v.push_back(kv.first);
    return v;
}

const std::vector<std::string>* ModStore::profile(const std::string& name) const {
    auto it = profiles_.find(name);
    return it == profiles_.end() ? nullptr : &it->second;
}

void ModStore::saveProfile(const std::string& name) {
    std::vector<std::string> list;
    for (const auto& m : mods_)
        if (m.enabled) list.push_back(m.folder);
    profiles_[name] = list;
    save();
}

bool ModStore::deleteProfile(const std::string& name) {
    bool r = profiles_.erase(name) > 0;
    if (r) save();
    return r;
}

int ModStore::applyProfile(const std::string& name, std::string& err) {
    auto it = profiles_.find(name);
    if (it == profiles_.end()) {
        err = "perfil inexistente";
        return 0;
    }
    std::set<std::string> want(it->second.begin(), it->second.end());
    int changed = 0;
    for (size_t i = 0; i < mods_.size(); i++) {
        bool should = want.count(mods_[i].folder) > 0;
        if (mods_[i].enabled == should) continue;
        std::string e;
        if (setEnabled(i, should, e)) changed++;
        else err = e;
    }
    return changed;
}

// ------------------------------------------------------------------ conflictos
static void walk(const std::string& base, const std::string& rel, const std::string& modName, std::map<std::string, std::vector<std::string>>& map, std::atomic<bool>* cancel) {
    std::string dir = rel.empty() ? base : util::join(base, rel);
    for (const auto& d : util::listDirs(dir)) {
        if (cancel && cancel->load()) return;
        walk(base, rel.empty() ? d : rel + "/" + d, modName, map, cancel);
    }
    if (rel.empty()) return;  // los archivos sueltos en la raíz del mod (config.json, preview.webp...) no son archivos del juego
    for (const auto& f : util::listFiles(dir)) {
        std::string key = util::lower(rel + "/" + f);
        auto& v = map[key];
        if (v.empty() || v.back() != modName) v.push_back(modName);
    }
}

std::vector<Conflict> scanConflicts(const std::vector<std::string>& folders, std::atomic<int>* done, std::atomic<bool>* cancel) {
    std::map<std::string, std::vector<std::string>> map;
    int n = 0;
    for (const auto& f : folders) {
        if (cancel && cancel->load()) break;
        walk(util::join(paths::mods(), f), "", f, map, cancel);
        if (done) done->store(++n);
    }
    std::vector<Conflict> out;
    for (auto& kv : map) {
        if (kv.second.size() < 2) continue;
        Conflict c;
        c.path = kv.first;
        c.mods = kv.second;
        out.push_back(std::move(c));
        if (out.size() >= 5000) break;
    }
    return out;
}
