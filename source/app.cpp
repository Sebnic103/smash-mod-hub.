#include "app.hpp"

#include <ctime>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "audio.hpp"
#include "theme.hpp"
#include "http.hpp"
#include "platform.hpp"
#include "settings.hpp"
#include "sys.hpp"
#include "util.hpp"

#ifndef APP_VERSION_STR
#define APP_VERSION_STR "1.3.0"
#endif

namespace col = ui::col;

namespace {

constexpr int W = 1280, H = 720, HEAD = 64, FOOT = 44, SUB = 36;
constexpr int COLS = 4, CW = 290, CH = 254, GAP = 20, X0 = 30;
const ui::Color kInk(15, 17, 24);

std::string count(int64_t n) {
    if (n < 0) return "-";
    char b[24];
    if (n >= 1000000) snprintf(b, sizeof b, "%.1fM", n / 1e6);
    else if (n >= 10000) snprintf(b, sizeof b, "%.1fk", n / 1e3);
    else snprintf(b, sizeof b, "%lld", (long long)n);
    return b;
}

std::string S(const char* es, const char* en) { return std::string(T(es, en)); }

std::string sortName(gb::Sort s) {
    switch (s) {
        case gb::Sort::New: return S("Nuevos", "Newest");
        case gb::Sort::Updated: return S("Actualizados", "Recently updated");
        default: return S("Destacados", "Featured");
    }
}

bool jobActive(const Job& j) {
    int s = j.state.load();
    return s == Job::Queued || (s >= Job::Downloading && s <= Job::Installing);
}

std::string fmtEta(uint64_t sec) {
    char b[32];
    if (sec >= 3600) snprintf(b, sizeof b, "%llu:%02llu:%02llu", (unsigned long long)(sec / 3600), (unsigned long long)(sec / 60 % 60), (unsigned long long)(sec % 60));
    else snprintf(b, sizeof b, "%llu:%02llu", (unsigned long long)(sec / 60), (unsigned long long)(sec % 60));
    return b;
}

std::string jobStateText(const Job& j) {
    int s = j.state.load();
    int pct = (int)(j.prog.fraction() * 100.0);
    char b[160];
    switch (s) {
        case Job::Queued: return S("En cola", "Queued");
        case Job::Downloading: {
            const uint64_t now = j.prog.now.load(), tot = j.prog.total.load(), bps = j.prog.bps.load();
            snprintf(b, sizeof b, "%s %d%%  (%s / %s)", T("Descargando", "Downloading"), pct, util::humanSize(now).c_str(), util::humanSize(tot).c_str());
            std::string out = b;
            if (bps > 0) {
                out += "  -  " + util::humanSize(bps) + "/s";
                if (tot > now) out += "  -  " + fmtEta((tot - now) / bps) + S(" restante", " left");
            }
            return out;
        }
        case Job::Verifying:
            snprintf(b, sizeof b, "%s %d%%", T("Verificando MD5", "Verifying MD5"), pct);
            return b;
        case Job::Extracting:
            snprintf(b, sizeof b, "%s %d%%", T("Extrayendo", "Extracting"), pct);
            return b;
        case Job::Installing: return S("Instalando", "Installing");
        case Job::Done: return S("Instalado", "Installed");
        case Job::Failed: return S("Error: ", "Error: ") + j.error;
        default: return S("Cancelado", "Cancelled");
    }
}

std::string stem(const std::string& file) {
    size_t d = file.find_last_of('.');
    return (d == std::string::npos || d == 0) ? file : file.substr(0, d);
}

std::string reslotProblemText(reslot::Problem p) {
    switch (p) {
        case reslot::Problem::NotFound: return S("No se encontró la carpeta del mod.", "The mod folder was not found.");
        case reslot::Problem::HasConfig: return S("Este mod trae un config.json con rutas de slot (slots extra o archivos compartidos). Reslotearlo desde aquí podría romperlo, así que no se toca.", "This mod ships a config.json with slot paths (extra slots or shared files). Reslotting it here could break it, so it is left alone.");
        case reslot::Problem::ExtraSlots: return S("Este mod usa slots a partir de c08 (slots extra), que necesitan configuración especial. No se puede reslotear desde aquí.", "This mod uses slots c08 and above (extra slots), which need special configuration. It cannot be reslotted here.");
        case reslot::Problem::NoSlots: return S("No se encontró ningún slot (cNN) en este mod: no es un mod de personaje o no hay nada que cambiar.", "No slot (cNN) was found in this mod: it is not a fighter mod or there is nothing to change.");
        case reslot::Problem::BadMap: return S("El nuevo slot choca con otro archivo del mod. Elige otro destino.", "The new slot clashes with another file in the mod. Pick a different target.");
        case reslot::Problem::Io: return S("Error al renombrar archivos.", "Error renaming files.");
        default: return "";
    }
}

std::string slotName(int s) {
    char b[8];
    snprintf(b, sizeof b, "c%02d", s);
    return b;
}

bool isArchiveName(const std::string& n) {
    std::string l = util::lower(n);
    for (const char* e : {".zip", ".7z", ".rar", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".tar.bz2"})
        if (util::endsWith(l, e)) return true;
    return false;
}

}  // namespace

// ============================================================================ ciclo de vida

bool App::init(SDL_Renderer* r) {
    ren_ = r;
    util::mkdirs(paths::data());
    util::setLogPath(paths::logFile());
    util::log("---- Smash Mod Hub %s ----", APP_VERSION_STR);
    for (const auto& d : {paths::mods(), paths::modsDisabled(), paths::downloads(), paths::thumbs(), paths::cache(), paths::importDir()}) util::mkdirs(d);
    util::rmrf(paths::tmp());  // restos de una sesión anterior
    util::mkdirs(paths::tmp());
    for (const auto& f : util::listFiles(paths::downloads()))
        if (util::endsWith(f, ".part")) util::rmrf(util::join(paths::downloads(), f));

    g_settings.load();
    gb::setGameId(g_settings.gameId);
    gb::loadCategoryCache();
    {  // un reslot interrumpido (apagón, cierre forzado): se termina antes de leer los mods
        std::string rerr;
        const int resumed = reslot::recover(util::join(paths::data(), "reslot_journal.json"), rerr);
        if (resumed != 0) util::log("reslot: recuperación -> %d operaciones (%s)", resumed, rerr.c_str());
    }
    http::init("romfs:/cacert.pem");
    img_.init(r, paths::cache());
    {  // fondo: una imagen propia (switch/smash-mod-hub/background.png|jpg) o el que genera la app
        std::string bg;
        for (const char* n : {"background.png", "background.jpg", "background.jpeg"}) {
            const std::string p = util::join(paths::data(), n);
            if (util::isFile(p)) {
                bg = p;
                break;
            }
        }
        theme::init(r, bg);
    }
    if (!audio::init(g_settings.musicMode, g_settings.volume)) util::log("música: sin audio");
    util::log("modo applet: %s", platform::appletMode() ? "sí (poca memoria; mejor abrir con un juego, R al lanzarlo)" : "no");
    store_.load();
    dl_.start();
    startedAt_ = sys::nowMs();
    refreshStatus();
    loadPage(1);  // la comprobación de actualizaciones espera unos segundos (ver frame): primero lo que se ve
    if (!http::tlsVerified()) toast(S("Aviso: sin certificados TLS (cacert.pem); las conexiones no se verifican", "Warning: no TLS certificates (cacert.pem); connections are not verified"), col::warn);
    return true;
}

void App::shutdown() {
    bgCancel_ = true;
    confCancel_ = true;
    rsCancel_ = true;
    http::abortAll();  // corta ya las peticiones en curso: salir no debe esperar a la red
    dl_.stop();
    spaceJob_.join();
    pageJob_.join();
    detailJob_.join();
    updJob_.join();
    updPrep_.join();
    sizeJob_.join();
    confJob_.join();
    catJob_.join();
    rsAnalyze_.join();
    rsApply_.join();
    audio::shutdown();
    theme::shutdown();
    img_.shutdown();
    store_.save();
    http::shutdown();
    util::flushLog();
}

void App::refreshStatus() {
    lastStatus_ = sys::nowMs();
    if (spaceJob_.busy.load()) return;
    spaceJob_.run([]() { return util::freeSpace(paths::root() + "/"); });  // en un hilo: statvfs en la SD tarda
}

void App::refreshImportCount() {
    int n = 0;
    for (const auto& f : util::listFiles(paths::importDir())) n += isArchiveName(f) ? 1 : 0;
    importCount_ = n;
    importDirty_ = false;
}

void App::toast(const std::string& msg, ui::Color c) {
    toastMsg_ = msg;
    toastCol_ = c;
    toastUntil_ = sys::nowMs() + 3500;
}

void App::confirm(const std::string& title, const std::string& msg, std::function<void()> onYes) {
    dialog_.open = true;
    dialog_.confirm = true;
    dialog_.title = title;
    dialog_.msg = msg;
    dialog_.onYes = std::move(onYes);
}

void App::notice(const std::string& title, const std::string& msg) {
    dialog_.open = true;
    dialog_.confirm = false;
    dialog_.title = title;
    dialog_.msg = msg;
    dialog_.onYes = nullptr;
}

void App::openView(const std::string& title, std::vector<std::string> lines) {
    view_.open = true;
    view_.title = title;
    view_.lines = std::move(lines);
    view_.scroll = 0;
}

std::string App::tabName(int t) {
    switch (t) {
        case TAB_BROWSE: return S("Explorar", "Browse");
        case TAB_INSTALLED: {
            int up = 0;
            for (const auto& m : store_.mods()) up += m.hasUpdate() ? 1 : 0;
            return S("Instalados", "Installed") + (up ? " *" : "");
        }
        case TAB_DOWNLOADS: {
            int n = dl_.activeCount();
            return S("Descargas", "Downloads") + (n ? " (" + std::to_string(n) + ")" : "");
        }
        case TAB_PROFILES: return S("Perfiles", "Profiles");
        default: return S("Herramientas", "Tools");
    }
}

// ============================================================================ tareas en segundo plano

void App::pollAsync() {
    uint64_t space = 0;
    if (spaceJob_.take(space)) freeSpace_ = space;

    gb::Page pg;
    if (pageJob_.take(pg)) {
        if (pg.ok) rememberPage(jobKey_, pg);
        if (jobKey_ == desiredKey_) {  // si el usuario ya pidió otra cosa, este resultado solo se guarda en caché
            wantPending_ = false;
            pageLoaded_ = true;
            if (pg.ok) {
                applyPage(std::move(pg));
            } else {
                pageErr_ = pg.err;
                pageNo_ = page_.page;
                if (!page_.items.empty()) toast(S("No se pudo cargar la página: ", "Could not load page: ") + pg.err, col::bad);
            }
        }
    }
    if (wantPending_ && !pageJob_.busy.load()) {
        wantPending_ = false;
        startPageJob();
    }

    if (detailPending_ && !detailJob_.busy.load()) {
        detailPending_ = false;
        gb::Item base = det_.item;
        detailJob_.run([base]() {
            DetailRes r;
            r.ok = gb::detail(base, r.d, r.err);
            return r;
        });
    }
    DetailRes dr;
    if (detailJob_.take(dr)) {
        if (detailOpen_ && dr.d.item.id == det_.item.id) {
            if (dr.ok) det_ = dr.d;
            else detErr_ = dr.err;
            detLoaded_ = true;
        }
    }

    UpdRes ur;
    if (updJob_.take(ur)) {
        for (const auto& p : ur.latest) store_.setLatest(p.first, p.second);
        store_.save();
        int up = 0;
        for (const auto& m : store_.mods()) up += m.hasUpdate() ? 1 : 0;
        if (updVerbose_ || up > 0) toast(up > 0 ? std::to_string(up) + S(" mod(s) con actualización disponible", " mod(s) have an update") : S("Todo está al día", "Everything is up to date"), up > 0 ? col::warn : col::good);
    }

    DetailRes pr;
    if (updPrep_.take(pr)) {
        if (pr.ok && !pr.d.files.empty()) queueNow(pr.d.item, pr.d.files[0], updFolder_, updTarget_);
        else toast(S("No se pudo obtener la actualización: ", "Could not get the update: ") + (pr.err.empty() ? S("sin archivos", "no files") : pr.err), col::bad);
    }

    SizeRes sr;
    if (sizeJob_.take(sr))
        for (const auto& p : sr.sizes) store_.setSize(p.first, p.second);

    ConflictRes cr;
    if (confJob_.take(cr)) {
        if (cr.cancelled) {
            toast(S("Búsqueda cancelada", "Scan cancelled"), col::dim);
        } else if (cr.list.empty()) {
            toast(S("No hay archivos repetidos entre los mods activados", "No duplicated files among enabled mods"), col::good);
        } else {
            std::vector<std::string> lines;
            lines.push_back(std::to_string(cr.list.size()) + S(" archivo(s) aparecen en más de un mod activado.", " file(s) appear in more than one enabled mod."));
            lines.push_back(S("ARCropolis no cargará esos archivos; desactiva o borra uno de los mods implicados.", "ARCropolis will not load those files; disable or delete one of the mods involved."));
            lines.push_back("");
            for (const auto& c : cr.list) {
                lines.push_back(c.path);
                std::string who = "    -> ";
                for (size_t i = 0; i < c.mods.size(); i++) who += (i ? ", " : "") + c.mods[i];
                lines.push_back(who);
            }
            openView(S("Conflictos entre mods", "Mod conflicts"), lines);
        }
    }

    CatRes cres;
    if (catJob_.take(cres) && cp_.open && cres.token == cp_.token) {
        cp_.loading = false;
        if (!cres.list.ok) {
            cp_.err = cres.list.err;
            if (cres.drill) toast(S("No se pudieron cargar las subcategorías: ", "Could not load subcategories: ") + cres.list.err + S("   (X: filtrar por ella)", "   (X: filter by it)"), col::bad);
        } else if (cres.drill && cres.list.cats.empty()) {  // sin subcategorías: se filtra por ella
            applyCategory(cres.target.id, catPathName(&cres.target));
        } else {
            CatLevel lv;
            lv.id = cres.parent;
            lv.name = cres.target.name;
            lv.cats = std::move(cres.list.cats);
            lv.learned = cres.list.learned;
            lv.stale = cres.list.stale;
            cp_.levels.push_back(std::move(lv));
            cp_.filter.clear();
            cp_.sel = 0;
            rebuildShown();
        }
    }
    if (cp_.open && cp_.pendingRoot && !catJob_.busy.load()) {
        cp_.pendingRoot = false;
        requestCats(nullptr);
    }

    ReslotAnalysisRes rar;
    if (rsAnalyze_.take(rar) && rs_.open && rs_.folder == rar.folder) {
        rs_.analyzing = false;
        rs_.an = rar.an;
        rs_.others = std::move(rar.others);
        if (rs_.an.ok()) {
            rs_.src = rs_.an.slots;
            rs_.dst = rs_.src;
            rs_.sel = 0;
        }
    }
    ReslotApplyRes rap;
    if (rsApply_.take(rap)) {
        rs_.applying = false;
        rs_.open = false;
        switch (rap.p) {
            case reslot::Problem::None:
                toast(S("Reslot terminado (", "Reslot done (") + std::to_string(rap.items) + S(" elementos). Reinicia el juego para aplicarlo", " items). Restart the game to apply it"), col::good);
                break;
            case reslot::Problem::Cancelled: toast(S("Reslot cancelado: no se cambió nada", "Reslot cancelled: nothing was changed"), col::dim); break;
            case reslot::Problem::Io: notice(S("Error al reslotear", "Reslot error"), rap.err + S("\nSe deshizo todo: el mod está como estaba.", "\nEverything was undone: the mod is as it was.")); break;
            default: notice(S("No se pudo reslotear", "Could not reslot"), reslotProblemText(rap.p)); break;
        }
    }

    if (sizesDirty_ && tab_ == TAB_INSTALLED) startSizeScan();
}

void App::handleFinishedJobs() {
    for (auto& j : dl_.takeFinished()) {
        if (j->state.load() == Job::Done) {
            for (const auto& f : j->folders) store_.recordInstall(f, j->meta);
            if (!j->meta.thumb.empty()) img_.invalidate(j->meta.thumb);
            store_.load();
            sizesDirty_ = true;
            toast(S("Instalado: ", "Installed: ") + j->item.name + S("  (reinicia el juego para aplicarlo)", "  (restart the game to apply)"), col::good);
        } else {
            toast(j->item.name + ": " + j->error, col::bad);
        }
    }
}

void App::startUpdateCheck(bool verbose) {
    if (updJob_.busy.load()) return;
    std::vector<std::pair<std::string, int64_t>> targets;
    for (const auto& m : store_.mods()) {
        if (!m.meta.gbId) continue;
        bool dup = false;
        for (const auto& t : targets) dup = dup || t.second == m.meta.gbId;
        if (!dup) targets.emplace_back(m.meta.model.empty() ? "Mod" : m.meta.model, m.meta.gbId);
    }
    if (targets.empty()) {
        if (verbose) toast(S("No hay mods de GameBanana instalados", "No GameBanana mods installed"), col::dim);
        return;
    }
    updVerbose_ = verbose;
    updDone_ = 0;
    updTotal_ = (int)targets.size();
    if (verbose) toast(S("Buscando actualizaciones...", "Checking for updates..."), col::dim);
    updJob_.run([this, targets]() {
        UpdRes r;
        for (const auto& t : targets) {
            if (bgCancel_.load()) break;
            int64_t latest = gb::latestFileId(t.first, t.second);
            if (latest) r.latest.emplace_back(t.second, latest);
            updDone_++;
            sys::sleepMs(120);  // no saturar la API
        }
        return r;
    });
}

void App::startSizeScan() {
    if (sizeJob_.busy.load()) return;
    std::vector<std::pair<std::string, std::string>> items;
    for (const auto& m : store_.mods()) items.emplace_back(m.folder, m.dir());
    sizesDirty_ = false;
    sizeJob_.run([items]() {
        SizeRes r;
        for (const auto& it : items) r.sizes.emplace_back(it.first, util::dirSize(it.second));
        return r;
    });
}

void App::startConflictScan() {
    if (confJob_.busy.load()) return;
    std::vector<std::string> folders;
    for (const auto& m : store_.mods())
        if (m.enabled) folders.push_back(m.folder);
    if (folders.size() < 2) {
        toast(S("Necesitas al menos 2 mods activados", "You need at least 2 enabled mods"), col::dim);
        return;
    }
    confDone_ = 0;
    confCancel_ = false;
    confTotal_ = (int)folders.size();
    confJob_.run([this, folders]() {
        ConflictRes r;
        r.list = scanConflicts(folders, &confDone_, &confCancel_);
        r.cancelled = confCancel_.load();
        return r;
    });
}

gb::Item App::itemFromMod(const Mod& m) {
    gb::Item it;
    it.id = m.meta.gbId;
    it.model = m.meta.model.empty() ? "Mod" : m.meta.model;
    it.name = m.title();
    it.author = m.author();
    it.version = m.version();
    return it;
}

void App::startModUpdate(size_t idx) {
    if (updPrep_.busy.load() || idx >= store_.mods().size()) return;
    const Mod& m = store_.mods()[idx];
    gb::Item base = itemFromMod(m);
    updFolder_ = m.folder;
    updTarget_ = m.enabled ? paths::mods() : paths::modsDisabled();
    toast(S("Preparando actualización de ", "Preparing update for ") + m.title(), col::dim);
    updPrep_.run([base]() {
        DetailRes r;
        r.ok = gb::detail(base, r.d, r.err);
        return r;
    });
}

void App::queueNow(const gb::Item& item, const gb::File& f, const std::string& replace, const std::string& target) {
    dl_.enqueue(item, f, replace, target);
    toast(S("En cola: ", "Queued: ") + item.name, col::good);
}

void App::queueDownload(const gb::Item& item, const gb::File& f) {
    for (const auto& j : dl_.jobs()) {
        if (f.id && j->file.id == f.id && jobActive(*j)) {
            toast(S("Ese archivo ya está en la cola", "That file is already queued"), col::warn);
            return;
        }
    }
    int idx = store_.findByGb(item.id);
    if (idx >= 0) {
        const Mod& m = store_.mods()[(size_t)idx];
        std::string replace = m.folder;
        std::string target = m.enabled ? paths::mods() : paths::modsDisabled();
        gb::Item it = item;
        gb::File ff = f;
        confirm(S("Mod ya instalado", "Mod already installed"), S("Se reemplazará la versión instalada de ", "The installed version of ") + it.name + S(" por este archivo.", " will be replaced with this file."), [this, it, ff, replace, target]() { queueNow(it, ff, replace, target); });
        return;
    }
    queueNow(item, f, "", paths::mods());
}

// ============================================================================ explorar

std::string App::pageKey(int page) const {
    return std::to_string(g_settings.gameId) + "|" + std::to_string(g_settings.perPage) + "|" + (query_.empty() ? std::string(gb::sortKey(sort_)) : "q:" + query_) + "|c" + std::to_string(catId_) + "|" + std::to_string(page);
}

void App::rememberPage(const std::string& key, const gb::Page& pg) {
    if (pageCache_.find(key) == pageCache_.end()) pageOrder_.push_back(key);
    pageCache_[key] = pg;
    while (pageOrder_.size() > 16) {  // las más antiguas fuera
        pageCache_.erase(pageOrder_.front());
        pageOrder_.erase(pageOrder_.begin());
    }
}

void App::applyPage(gb::Page&& pg) {
    page_ = std::move(pg);
    pageLoaded_ = true;
    pageNo_ = page_.page;
    pageErr_.clear();
    sel_ = 0;
    browseScroll_.reset();
}

// Pide una página. Si ya se vio, se muestra al instante. Si hay otra carga en marcha, esta queda pendiente y
// la anterior se descarta al llegar (antes se ignoraba la petición y se perdía, p. ej. una búsqueda).
void App::loadPage(int page) {
    const int p = std::max(1, page);
    desiredPage_ = p;
    desiredKey_ = pageKey(p);
    pageErr_.clear();
    if (pageJob_.busy.load() && jobKey_ != desiredKey_) gb::cancelSearch();  // la carga en marcha ya no interesa: que pare
    auto it = pageCache_.find(desiredKey_);
    if (it != pageCache_.end()) {
        wantPending_ = false;
        gb::Page copy = it->second;
        applyPage(std::move(copy));
        return;
    }
    wantPending_ = true;  // pollAsync la lanza en cuanto no haya otra en curso
}

void App::startPageJob() {
    jobKey_ = desiredKey_;
    std::string q = query_;
    gb::Sort s = sort_;
    int p = desiredPage_;
    int per = g_settings.perPage;
    const int64_t cat = catId_;
    pageJob_.run([q, s, p, per, cat]() {
        if (q.empty()) return cat > 0 ? gb::browseCategory(cat, p, s, per) : gb::browse(p, s, per);
        return cat > 0 ? gb::searchInCategory(cat, q) : gb::search(q, p, per);
    });
}

bool App::moveSel(int& sel, int count, int cols, const Input& in) {
    int old = sel;
    if (count <= 0) {
        sel = 0;
        return false;
    }
    if (sel >= count) sel = count - 1;
    if (in.left && sel % cols > 0) sel--;
    if (in.right && sel % cols < cols - 1 && sel + 1 < count) sel++;
    if (in.up && sel - cols >= 0) sel -= cols;
    if (in.down) {
        if (sel + cols < count) sel += cols;
        else if (sel / cols < (count - 1) / cols) sel = count - 1;
    }
    return sel != old;
}

void App::ensureVisible(int sel, int cols, Scroller& s, int viewH) {
    int top = (sel / cols) * (CH + GAP);
    int bottom = top + CH;
    if (top < s.target) s.target = (float)top;
    if (bottom > s.target + viewH) s.target = (float)(bottom - viewH);
    if (s.target < 0) s.target = 0;
}

void App::updateBrowse(const Input& in) {
    const auto& items = page_.items;  // sigue siendo navegable mientras carga la página siguiente
    if (!items.empty()) {
        moveSel(sel_, (int)items.size(), COLS, in);
        ensureVisible(sel_, COLS, browseScroll_, H - FOOT - (HEAD + SUB) - 10);
        if (in.a) openDetail(items[(size_t)sel_]);
    } else if (in.a && !pageJob_.busy.load() && !wantPending_) {
        loadPage(pageNo_);
    }
    if (in.y) {
        askText(S("Buscar mods", "Search mods"), query_, 50, false, [this](const std::string& text) {
            const std::string q = util::trim(text);
            if (q == query_) return;
            query_ = q;
            loadPage(1);
        });
        return;
    }
    if (in.x) {
        if (!query_.empty()) {
            toast(S("El orden no se aplica a las búsquedas", "Sorting does not apply to searches"), col::dim);
        } else {
            sort_ = sort_ == gb::Sort::Default ? gb::Sort::New : sort_ == gb::Sort::New ? gb::Sort::Updated : gb::Sort::Default;
            loadPage(1);
        }
    }
    if (in.b) openCatPicker();
    if (in.minus) {
        query_.clear();
        catId_ = 0;
        catName_.clear();
        pageCache_.clear();  // "Inicio" también sirve para refrescar
        pageOrder_.clear();
        loadPage(1);
    }
    const bool loadingNow = pageJob_.busy.load() || wantPending_;
    const int cur = loadingNow ? desiredPage_ : pageNo_;
    if (in.zr && !items.empty() && !(page_.complete && cur == pageNo_)) loadPage(cur + 1);
    if (in.zl && cur > 1) loadPage(cur - 1);
}

void App::drawBrowse() {
    const int areaY = HEAD + SUB, areaH = H - FOOT - areaY;
    theme::subbar(HEAD, SUB, col::accent);
    std::string left = query_.empty() ? S("Orden: ", "Sort: ") + sortName(sort_) : S("Búsqueda: ", "Search: ") + "\"" + query_ + "\"";
    if (catId_ > 0) left += "   -   " + S("Filtro: ", "Filter: ") + (catName_.empty() ? S("(categoría)", "(category)") : catName_);
    ui::text(left, 30, HEAD + 6, ui::SMALL, catId_ > 0 ? col::accent : col::text, 700);
    const bool loadingNow = pageJob_.busy.load() || wantPending_;
    const int shownPage = loadingNow ? desiredPage_ : pageNo_;
    std::string right = S("Página ", "Page ") + std::to_string(shownPage);
    {
        const int per = std::max(1, g_settings.perPage);
        const int pages = page_.total > 0 ? (int)((page_.total + per - 1) / per) : 0;
        if (!loadingNow && pages >= pageNo_ && (int)page_.items.size() <= per && pages > 1) right += S(" de ", " of ") + std::to_string(pages);
    }
    if (!page_.items.empty()) right += "   -   " + std::to_string(page_.items.size()) + S(" mods", " mods");
    if (page_.scanned > 0 && !loadingNow) right = S("Buscado en ", "Searched ") + std::to_string(page_.scanned) + (catId_ > 0 ? S(" mods de la categoría", " mods in the category") : S(" mods recientes", " recent mods"));
    ui::text(right, W - 530, HEAD + 6, ui::SMALL, col::dim, 500, 2);

    bool busy = loadingNow;
    if (page_.items.empty()) {
        if (busy || !pageLoaded_) {
            std::string sub = S("Conectando con GameBanana", "Connecting to GameBanana");
            int done = 0, total = 0;
            gb::searchProgress(done, total);
            if (!query_.empty() && total > 0) sub = S("Buscando \"", "Searching \"") + query_ + "\"   -   " + S("página ", "page ") + std::to_string(std::min(done + 1, total)) + "/" + std::to_string(total);
            else if (!query_.empty()) sub = S("Buscando \"", "Searching \"") + query_ + "\" " + S("en GameBanana", "on GameBanana");
            drawCenterMessage((query_.empty() ? S("Cargando", "Loading") : S("Buscando", "Searching")) + std::string((size_t)((now_ / 350) % 4), '.'), sub, col::text);
        } else if (!pageErr_.empty()) {
            drawCenterMessage(S("No se pudo conectar", "Could not connect"), pageErr_ + "   -   " + S("A: reintentar", "A: retry"), col::bad);
        } else {
            std::string sub = page_.dropped > 0 ? S("Se descartaron resultados de otro juego: revisa el ID de juego en Herramientas", "Results from another game were dropped: check the game ID in Tools") : S("Prueba otra búsqueda, otro filtro (B) o pulsa - para volver al inicio", "Try another search, another filter (B) or press - to go back");
            drawCenterMessage(S("Sin resultados", "No results"), sub, col::dim);
        }
        return;
    }

    ui::clip(0, areaY, W, areaH);
    const int pitch = CH + GAP;
    for (size_t i = 0; i < page_.items.size(); i++) {
        int row = (int)i / COLS, cl = (int)i % COLS;
        int x = X0 + cl * (CW + GAP);
        int y = areaY + 10 + row * pitch - (int)browseScroll_.cur;
        if (y + CH < areaY || y > areaY + areaH) continue;
        const gb::Item& it = page_.items[i];
        Card c;
        std::string url = it.thumbUrl();
        c.noImage = url.empty();
        c.tex = c.noImage ? nullptr : img_.get(url, true);
        c.title = it.name;
        c.l2 = it.author;
        c.l3 = it.category();
        c.selected = (int)i == sel_;
        int mi = store_.findByGb(it.id);
        if (mi >= 0) {
            c.badges.push_back({S("INSTALADO", "INSTALLED"), col::good});
            if (store_.mods()[(size_t)mi].hasUpdate()) c.badges.push_back({S("ACTUALIZAR", "UPDATE"), col::warn});
        }
        drawCard(x, y, CW, CH, c);
    }
    ui::unclip();
    ui::scrollbar(W - 14, areaY + 10, areaH - 20, areaH - 10, ((int)page_.items.size() + COLS - 1) / COLS * pitch, (int)browseScroll_.cur);
    if (busy) ui::text(S("Cargando", "Loading") + std::string((size_t)((now_ / 350) % 4), '.'), W - 230, HEAD + 6, ui::SMALL, col::accent, 200, 2);
}

// ============================================================================ filtros por categoría

std::string App::catPathName(const gb::Category* extra) const {
    std::string n;
    for (size_t i = 1; i < cp_.levels.size(); i++) n += (n.empty() ? "" : " / ") + cp_.levels[i].name;
    if (extra) n += (n.empty() ? "" : " / ") + extra->name;
    return n;
}

void App::openCatPicker() {
    cp_ = CatPicker();
    cp_.open = true;
    cp_.token = ++catToken_;
    if (catJob_.busy.load()) {  // todavía acaba una petición anterior: la raíz se pide en cuanto termine
        cp_.loading = true;
        cp_.pendingRoot = true;
        return;
    }
    requestCats(nullptr);
}

void App::requestCats(const gb::Category* target) {
    if (catJob_.busy.load()) {
        toast(S("Espera un momento...", "Wait a moment..."), col::dim);
        return;
    }
    cp_.loading = true;
    cp_.err.clear();
    const int64_t parent = target ? target->id : 0;
    const bool drill = target != nullptr;
    const gb::Category t = target ? *target : gb::Category();
    const int token = cp_.token;
    catJob_.run([parent, drill, t, token]() {
        CatRes r;
        r.parent = parent;
        r.drill = drill;
        r.target = t;
        r.token = token;
        r.list = gb::categories(parent);
        return r;
    });
}

void App::rebuildShown() {
    cp_.shown.clear();
    if (cp_.levels.empty()) return;
    const auto& cats = cp_.levels.back().cats;
    const std::string f = util::lower(cp_.filter);
    for (size_t i = 0; i < cats.size(); i++)
        if (f.empty() || util::lower(cats[i].name).find(f) != std::string::npos) cp_.shown.push_back((int)i);
    if (cp_.sel > (int)cp_.shown.size()) cp_.sel = (int)cp_.shown.size();
}

void App::applyCategory(int64_t id, const std::string& name) {
    cp_.open = false;
    cp_.token = ++catToken_;  // cualquier respuesta pendiente del selector se descarta
    catId_ = id > 0 ? id : 0;
    catName_ = catId_ ? name : std::string();
    loadPage(1);
}

namespace {
constexpr int kCatRows = 14, kCatRowH = 36;
}

void App::updateCatPicker(const Input& in) {
    if (in.b) {
        if (!cp_.loading && cp_.levels.size() > 1) {  // un nivel atrás
            cp_.levels.pop_back();
            cp_.filter.clear();
            cp_.err.clear();
            cp_.sel = 0;
            rebuildShown();
        } else {
            cp_.open = false;
            cp_.token = ++catToken_;
        }
        return;
    }
    if (cp_.loading) return;  // esperando a GameBanana
    if (cp_.levels.empty()) {  // la raíz falló: A reintenta
        if (in.a) requestCats(nullptr);
        return;
    }
    const int n = 1 + (int)cp_.shown.size();
    moveSel(cp_.sel, n, 1, in);
    if (in.zl) cp_.sel = std::max(0, cp_.sel - kCatRows);
    if (in.zr) cp_.sel = std::min(n - 1, cp_.sel + kCatRows);
    if (in.y) {
        askText(S("Buscar en la lista de categorías", "Search the category list"), cp_.filter, 40, false, [this](const std::string& text) {
            if (!cp_.open) return;
            cp_.filter = util::trim(text);
            cp_.sel = 0;
            rebuildShown();
        });
        return;
    }
    if (!(in.a || in.x)) return;
    const CatLevel& lv = cp_.levels.back();
    if (cp_.sel == 0) {  // fila especial: quitar el filtro (raíz) o filtrar por todo lo de esta categoría
        if (lv.id == 0) applyCategory(0, "");
        else applyCategory(lv.id, catPathName(nullptr));
        return;
    }
    const gb::Category c = lv.cats[(size_t)cp_.shown[(size_t)cp_.sel - 1]];
    if (in.x) applyCategory(c.id, catPathName(&c));  // X: filtrar ya, sin abrir sus subcategorías
    else requestCats(&c);                            // A: abrir (si no tiene subcategorías, filtra)
}

void App::drawCatPicker() {
    ui::fill(0, 0, W, H, ui::Color(0, 0, 0, 200));
    const int x = 50, y = 30, w = W - 100, h = H - 60;
    theme::panel(x, y, w, h, col::accent);
    std::string title = S("Filtrar por categoría", "Filter by category");
    const std::string path = catPathName(nullptr);
    if (!path.empty()) title += ":  " + path;
    ui::text(title, x + 28, y + 20, ui::BIG, col::text, w - 56 - 220);
    const int ay = y + 78;
    if (cp_.levels.empty()) {
        if (cp_.loading) ui::text(S("Cargando categorías", "Loading categories") + std::string((size_t)((now_ / 350) % 4), '.'), x, ay + 160, ui::NORMAL, col::text, w, 1);
        else ui::text(S("No se pudieron cargar las categorías: ", "Could not load categories: ") + cp_.err + "   -   " + S("A: reintentar", "A: retry"), x + 30, ay + 160, ui::NORMAL, col::bad, w - 60, 1);
    } else {
        const CatLevel& lv = cp_.levels.back();
        std::string note;
        if (lv.learned) note = S("Lista parcial: solo categorías vistas en los mods ya cargados", "Partial list: only categories seen in the mods already loaded");
        else if (lv.stale) note = S("Sin conexión: lista guardada de la última vez", "Offline: list saved from last time");
        if (!cp_.filter.empty()) note += (note.empty() ? "" : "   -   ") + S("Texto: \"", "Text: \"") + cp_.filter + "\"";
        if (!note.empty()) ui::text(note, x + 28, y + 56, ui::SMALL, col::warn, w - 56);
        const int n = 1 + (int)cp_.shown.size();
        const int first = std::max(0, std::min(cp_.sel - kCatRows / 2, n - kCatRows));
        for (int i = first; i < std::min(n, first + kCatRows); i++) {
            const int ry = ay + (i - first) * kCatRowH;
            const bool sel = i == cp_.sel;
            theme::row(x + 24, ry, w - 68, kCatRowH - 4, sel, col::accent);
            if (i == 0) {
                ui::text(lv.id == 0 ? S("Todas las categorías (quitar filtro)", "All categories (remove filter)") : S("Todo en ", "Everything in ") + lv.name, x + 44, ry + 3, ui::NORMAL, col::accent, w - 130);
            } else {
                const gb::Category& c = lv.cats[(size_t)cp_.shown[(size_t)i - 1]];
                ui::text(c.name, x + 44, ry + 3, ui::NORMAL, sel ? col::white : col::text, w - 310);
                if (c.count >= 0) ui::text(count(c.count), x + w - 240, ry + 3, ui::NORMAL, col::dim, 160, 2);
            }
        }
        if (cp_.shown.empty() && !cp_.filter.empty()) ui::text(S("Sin coincidencias", "No matches"), x, ay + 60, ui::NORMAL, col::dim, w, 1);
        ui::scrollbar(x + w - 28, ay, kCatRows * kCatRowH, kCatRows * kCatRowH, n * kCatRowH, first * kCatRowH);
        if (cp_.loading) ui::text(S("Cargando", "Loading") + std::string((size_t)((now_ / 350) % 4), '.'), x + w - 250, y + 26, ui::NORMAL, col::accent, 220, 2);
    }
    theme::hints(S("A Abrir   X Filtrar aquí   Y Buscar   B Atrás   ZL/ZR Saltar", "A Open   X Filter here   Y Search   B Back   ZL/ZR Jump"), x + 28, y + h - 42, w - 56);
}

// ============================================================================ detalle

void App::openDetail(const gb::Item& it) {
    det_ = gb::Detail();
    det_.item = it;
    detErr_.clear();
    detLoaded_ = false;
    detFile_ = 0;
    detImg_ = 0;
    detTextScroll_ = 0;
    detailOpen_ = true;
    detailPending_ = true;
}

void App::updateDetail(const Input& in) {
    if (in.b) {
        detailOpen_ = false;
        return;
    }
    if (in.left && detImg_ > 0) detImg_--;
    if (in.right && detImg_ + 1 < (int)det_.item.images.size()) detImg_++;
    int nf = (int)det_.files.size();
    if (in.up && detFile_ > 0) detFile_--;
    if (in.down && detFile_ + 1 < nf) detFile_++;
    if (in.stickY != 0.f) detTextScroll_ = std::max(0.f, detTextScroll_ + in.stickY * 16.f);
    if (in.a) {
        if (!detLoaded_) return;
        if (nf > 0) {
            queueDownload(det_.item, det_.files[(size_t)detFile_]);
        } else {  // error o sin archivos: reintentar
            detErr_.clear();
            detLoaded_ = false;
            detailPending_ = true;
        }
    }
}

void App::drawDetail() {
    ui::fill(0, 0, W, H, ui::Color(10, 11, 17, 205));  // semitransparente: se sigue viendo el fondo del tema
    ui::fill(0, 0, W, HEAD, col::header);
    ui::fill(0, HEAD - 2, W, 2, col::accent.withAlpha(80));
    theme::slant(-30, 0, 62, HEAD - 2, 20, col::accent);
    ui::text(det_.item.name, 50, 12, ui::BIG, col::text, W - 90);

    // --- imagen
    const int lx = 40, ly = HEAD + 16, lw = 640, lh = 360;
    theme::shadow(lx, ly, lw, lh, 8);
    ui::fill(lx, ly, lw, lh, col::placeholder);
    std::string url = det_.item.galleryUrl((size_t)detImg_);
    SDL_Texture* t = url.empty() ? nullptr : img_.get(url, true);
    if (t) ui::imageContain(t, lx, ly, lw, lh);
    else ui::text(url.empty() ? S("Sin imagen", "No image") : S("Cargando imagen...", "Loading image..."), lx, ly + lh / 2 - 12, ui::NORMAL, col::dim, lw, 1);
    ui::outline(lx, ly, lw, lh, col::panelHi, 2);
    if (det_.item.images.size() > 1) {
        std::string n = std::to_string(detImg_ + 1) + " / " + std::to_string(det_.item.images.size());
        int tw = ui::textWidth(n, ui::SMALL) + 56;
        theme::slant(lx + lw - tw - 10, ly + 10, tw, 28, 8, ui::Color(8, 9, 14, 220));
        ui::text("<", lx + lw - tw + 2, ly + 13, ui::SMALL, detImg_ > 0 ? col::accent : col::dim, 24, 1);
        ui::text(n, lx + lw - tw + 14, ly + 13, ui::SMALL, col::text, tw - 28, 1);
        ui::text(">", lx + lw - 38, ly + 13, ui::SMALL, detImg_ + 1 < (int)det_.item.images.size() ? col::accent : col::dim, 24, 1);
    }

    // --- descripción (se desplaza con el stick derecho)
    const int dy = ly + lh + 14, dh = H - FOOT - dy - 6;
    theme::shadow(lx, dy, lw, dh, 5);
    ui::fill(lx, dy, lw, dh, col::panel);
    ui::fill(lx, dy, lw, 3, col::accent.withAlpha(110));
    if (!det_.text.empty()) {
        const int tw = lw - 24;
        int total = ui::textWrapped(det_.text, lx + 12, dy + 8, tw, ui::SMALL, col::text, (int)detTextScroll_, dh - 16);
        float maxScroll = (float)std::max(0, total - (dh - 16));
        if (detTextScroll_ > maxScroll) detTextScroll_ = maxScroll;
        if (maxScroll > 0) ui::text(S("stick der.: desplazar", "R stick: scroll"), lx, dy + dh - 22, ui::SMALL, col::dim.withAlpha(180), lw - 12, 2);
    } else {
        ui::text(detLoaded_ ? S("(sin descripción)", "(no description)") : S("Cargando descripción...", "Loading description..."), lx + 12, dy + 10, ui::SMALL, col::dim);
    }

    // --- datos
    const int rx = 710, rw = 530;
    int y = HEAD + 16;
    const gb::Item& it = det_.item;
    auto line = [&](const std::string& s, ui::Color c) {
        if (!s.empty()) {
            ui::text(s, rx, y, ui::NORMAL, c, rw);
            y += 28;
        }
    };
    line(S("Autor: ", "Author: ") + (it.author.empty() ? "-" : it.author), col::text);
    line(S("Categoría: ", "Category: ") + (it.category().empty() ? "-" : it.category()), col::dim);
    std::string dates;
    int64_t added = it.added, upd = it.updated ? it.updated : it.modified;
    if (added) dates += S("Subido ", "Added ") + util::formatDate(added);
    if (upd && upd != added) dates += (dates.empty() ? "" : "   ") + S("Actualizado ", "Updated ") + util::formatDate(upd);
    line(dates, col::dim);
    std::string stats;
    if (det_.downloads >= 0) stats += S("Descargas: ", "Downloads: ") + count(det_.downloads) + "   ";
    if (it.likes >= 0) stats += S("Me gusta: ", "Likes: ") + count(it.likes) + "   ";
    if (it.views >= 0) stats += S("Vistas: ", "Views: ") + count(it.views);
    line(stats, col::dim);
    int mi = store_.findByGb(it.id);
    if (mi >= 0) {
        const Mod& m = store_.mods()[(size_t)mi];
        line(S("Instalado en: ", "Installed as: ") + m.folder + (m.enabled ? "" : S("  (desactivado)", "  (disabled)")), col::good);
    }

    // --- archivos
    y += 8;
    theme::slant(rx, y + 4, 8, 22, 0, col::accent);
    ui::text(S("Archivos", "Files"), rx + 18, y, ui::NORMAL, col::accent);
    y += 36;
    const int rowH = 54, maxRows = 5;
    if (!detLoaded_) {
        ui::text(S("Cargando archivos", "Loading files") + std::string((size_t)((now_ / 350) % 4), '.'), rx, y, ui::NORMAL, col::dim);
    } else if (det_.files.empty()) {
        ui::text(detErr_.empty() ? S("Este mod no tiene archivos descargables", "This mod has no downloadable files") : S("Error: ", "Error: ") + detErr_, rx, y, ui::SMALL, col::bad, rw);
        ui::text(S("A: reintentar", "A: retry"), rx, y + 26, ui::SMALL, col::dim);
    } else {
        int n = (int)det_.files.size();
        int first = std::max(0, std::min(detFile_ - maxRows / 2, n - maxRows));
        for (int i = first; i < std::min(n, first + maxRows); i++) {
            const gb::File& f = det_.files[(size_t)i];
            int ry = y + (i - first) * rowH;
            bool sel = i == detFile_;
            theme::row(rx, ry, rw, rowH - 6, sel, col::accent);
            bool installed = mi >= 0 && store_.mods()[(size_t)mi].meta.fileId == f.id;
            const int bw = installed ? ui::textWidth(S("INSTALADO", "INSTALLED"), ui::SMALL) + 24 : 0;
            ui::text(f.name, rx + 18, ry + 2, ui::NORMAL, sel ? col::white : col::text, rw - 30 - (installed ? bw + 6 : 0));
            std::string sub = util::humanSize(f.size);
            if (f.added) sub += "  -  " + util::formatDate(f.added);
            if (!f.desc.empty()) sub += "  -  " + f.desc;
            ui::text(sub, rx + 18, ry + 27, ui::SMALL, col::dim, rw - 30);
            if (installed) {
                theme::slant(rx + rw - bw - 8, ry + 5, bw, 22, 8, col::good);
                ui::text(S("INSTALADO", "INSTALLED"), rx + rw - bw - 8, ry + 5, ui::SMALL, kInk, bw, 1);
            }
        }
        if (n > maxRows) ui::text(std::to_string(detFile_ + 1) + " / " + std::to_string(n), rx, y + maxRows * rowH, ui::SMALL, col::dim, rw, 2);

        // botón de descarga
        const gb::File& f = det_.files[(size_t)detFile_];
        const int by = H - FOOT - 70;
        theme::shadow(rx, by, rw, 56, 6);
        ui::fill(rx, by, rw, 56, col::accent);
        ui::fill(rx, by, rw, 2, ui::Color(255, 255, 255, 60));
        ui::fill(rx + 14, by + 12, 32, 32, ui::Color(0, 0, 0, 70));
        ui::text("A", rx + 14, by + 16, ui::NORMAL, col::white, 32, 1);
        ui::text((mi >= 0 ? S("Reemplazar", "Replace") : S("Descargar", "Download")) + "  (" + util::humanSize(f.size) + ")", rx + 58, by + 14, ui::NORMAL, col::white, rw - 72);
    }
    drawFooter(S("A Descargar   B Volver   Izq/Der Capturas   Arr/Abj Archivo   Stick Texto", "A Download   B Back   Left/Right Screenshots   Up/Down File   Stick Text"));
}

// ============================================================================ instalados

void App::updateInstalled(const Input& in) {
    auto& mods = store_.mods();
    if (in.minus) startUpdateCheck(true);
    if (in.zl && !mods.empty()) confirm(S("Desactivar todos", "Disable all"), S("Se moverán todos los mods a la carpeta de desactivados.", "All mods will be moved to the disabled folder."), [this]() {
        std::string err;
        int n = store_.setAll(false, err);
        toast(std::to_string(n) + S(" mod(s) desactivados", " mod(s) disabled"), err.empty() ? col::good : col::bad);
    });
    if (in.zr && !mods.empty()) confirm(S("Activar todos", "Enable all"), S("Se activarán todos los mods instalados.", "All installed mods will be enabled."), [this]() {
        std::string err;
        int n = store_.setAll(true, err);
        toast(std::to_string(n) + S(" mod(s) activados", " mod(s) enabled"), err.empty() ? col::good : col::bad);
    });
    if (mods.empty()) return;
    moveSel(instSel_, (int)mods.size(), COLS, in);
    ensureVisible(instSel_, COLS, instScroll_, H - FOOT - (HEAD + SUB) - 10);
    const Mod& m = mods[(size_t)instSel_];
    if (in.a) {
        std::string err;
        bool target = !m.enabled;
        if (store_.setEnabled((size_t)instSel_, target, err)) toast(m.title() + (target ? S(": activado", ": enabled") : S(": desactivado", ": disabled")), target ? col::good : col::dim);
        else toast(err, col::bad);
    }
    if (in.x) {
        size_t idx = (size_t)instSel_;
        confirm(S("Borrar mod", "Delete mod"), S("¿Borrar \"", "Delete \"") + m.title() + S("\" de la tarjeta SD? No se puede deshacer.", "\" from the SD card? This cannot be undone."), [this, idx]() {
            std::string err;
            std::string name = store_.mods()[idx].title();
            if (store_.remove(idx, err)) {
                toast(S("Borrado: ", "Deleted: ") + name, col::good);
                if (instSel_ >= (int)store_.mods().size()) instSel_ = std::max(0, (int)store_.mods().size() - 1);
            } else {
                toast(err, col::bad);
            }
        });
    }
    if (in.b) openReslot((size_t)instSel_);
    if (in.y) {
        if (m.hasUpdate()) startModUpdate((size_t)instSel_);
        else if (m.meta.gbId) openDetail(itemFromMod(m));
        else toast(S("Este mod no viene de GameBanana", "This mod did not come from GameBanana"), col::dim);
    }
}

void App::drawInstalled() {
    const int areaY = HEAD + SUB, areaH = H - FOOT - areaY;
    theme::subbar(HEAD, SUB, col::accent);
    const auto& mods = store_.mods();
    int up = 0;
    for (const auto& m : mods) up += m.hasUpdate() ? 1 : 0;
    std::string left = std::to_string(mods.size()) + S(" mods   -   ", " mods   -   ") + std::to_string(store_.enabledCount()) + S(" activados", " enabled");
    if (up) left += "   -   " + std::to_string(up) + S(" con actualización", " with updates");
    ui::text(left, 30, HEAD + 6, ui::SMALL, col::text, 800);
    if (updJob_.busy.load()) ui::text(S("Buscando actualizaciones ", "Checking updates ") + std::to_string(updDone_.load()) + "/" + std::to_string(updTotal_), W - 430, HEAD + 6, ui::SMALL, col::accent, 400, 2);

    if (mods.empty()) {
        drawCenterMessage(S("No hay mods instalados", "No mods installed"), S("Descarga alguno desde Explorar, o copia .zip a switch/smash-mod-hub/import", "Download one from Browse, or copy .zip files to switch/smash-mod-hub/import"), col::dim);
        return;
    }
    ui::clip(0, areaY, W, areaH);
    const int pitch = CH + GAP;
    for (size_t i = 0; i < mods.size(); i++) {
        int row = (int)i / COLS, cl = (int)i % COLS;
        int x = X0 + cl * (CW + GAP);
        int y = areaY + 10 + row * pitch - (int)instScroll_.cur;
        if (y + CH < areaY || y > areaY + areaH) continue;
        const Mod& m = mods[i];
        Card c;
        std::string path = m.imagePath();
        c.noImage = path.empty();
        c.tex = c.noImage ? nullptr : img_.get(path, false);
        c.title = m.title();
        c.l2 = m.author() + (m.version().empty() ? "" : "  v" + m.version());
        std::string cat = m.category();
        c.l3 = cat + (m.sizeKnown ? (cat.empty() ? "" : "  -  ") + util::humanSize(m.size) : "");
        c.selected = (int)i == instSel_;
        c.dim = !m.enabled;
        c.badges.push_back(m.enabled ? Badge{"ON", col::good} : Badge{"OFF", col::bad});
        if (m.hasUpdate()) c.badges.push_back({S("ACTUALIZAR", "UPDATE"), col::warn});
        drawCard(x, y, CW, CH, c);
    }
    ui::unclip();
    ui::scrollbar(W - 14, areaY + 10, areaH - 20, areaH - 10, ((int)mods.size() + COLS - 1) / COLS * pitch, (int)instScroll_.cur);
}

// ============================================================================ reslotter

void App::openReslot(size_t idx) {
    if (idx >= store_.mods().size() || rsAnalyze_.busy.load() || rsApply_.busy.load()) return;
    if (dl_.activeCount() > 0) {
        toast(S("Espera a que terminen las descargas para reslotear", "Wait for the downloads to finish before reslotting"), col::warn);
        return;
    }
    const Mod& m = store_.mods()[idx];
    rs_ = ReslotUi();
    rs_.open = true;
    rs_.analyzing = true;
    rs_.folder = m.folder;
    rs_.title = m.title();
    rs_.dir = m.dir();
    std::vector<std::pair<std::string, std::string>> others;  // (título, carpeta) de los demás mods activados
    for (const auto& o : store_.mods())
        if (o.enabled && o.folder != m.folder) others.emplace_back(o.title(), o.dir());
    const std::string folder = m.folder, dir = m.dir();
    rsAnalyze_.run([this, folder, dir, others]() {
        ReslotAnalysisRes r;
        r.folder = folder;
        r.an = reslot::analyze(dir, &bgCancel_);
        if (r.an.ok()) {
            for (const auto& o : others) {
                if (bgCancel_.load()) break;
                ReslotOther ro;
                ro.title = o.first;
                ro.use = reslot::fighterSlots(o.second);
                if (!ro.use.empty()) r.others.push_back(std::move(ro));
            }
        }
        return r;
    });
}

bool App::reslotValid() const {
    std::vector<int> seen;
    bool changed = false;
    for (size_t i = 0; i < rs_.dst.size(); i++) {
        if (rs_.dst[i] < 0 || rs_.dst[i] > reslot::kMaxSlot) return false;
        if (std::find(seen.begin(), seen.end(), rs_.dst[i]) != seen.end()) return false;
        seen.push_back(rs_.dst[i]);
        changed = changed || rs_.dst[i] != rs_.src[i];
    }
    return changed;
}

// Otros mods activados que ya usan el personaje y el slot al que se va a mover este.
std::vector<std::string> App::reslotWarnings() const {
    std::vector<std::string> out;
    for (size_t i = 0; i < rs_.src.size() && i < rs_.dst.size(); i++) {
        if (rs_.dst[i] == rs_.src[i]) continue;
        for (const auto& f : rs_.an.perFighter) {
            if (std::find(f.second.begin(), f.second.end(), rs_.src[i]) == f.second.end()) continue;
            for (const auto& o : rs_.others) {
                auto it = o.use.find(f.first);
                if (it == o.use.end() || std::find(it->second.begin(), it->second.end(), rs_.dst[i]) == it->second.end()) continue;
                const std::string w = "\"" + o.title + S("\" ya usa ", "\" already uses ") + f.first + " " + slotName(rs_.dst[i]);
                if (std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
            }
        }
    }
    return out;
}

void App::startReslot() {
    if (!reslotValid()) {
        toast(S("Elige slots distintos y cambia al menos uno", "Pick different slots and change at least one"), col::warn);
        return;
    }
    reslot::SlotMap map;
    std::string summary;
    for (size_t i = 0; i < rs_.src.size(); i++) {
        if (rs_.src[i] == rs_.dst[i]) continue;
        map[rs_.src[i]] = rs_.dst[i];
        summary += (summary.empty() ? "" : ",  ") + slotName(rs_.src[i]) + " -> " + slotName(rs_.dst[i]);
    }
    std::string msg = summary + "\n" + std::to_string(rs_.an.items) + S(" elementos llevan el slot en el nombre.", " items carry the slot in their name.");
    const auto warns = reslotWarnings();
    if (!warns.empty()) msg += "\n" + S("Aviso: ", "Warning: ") + warns[0] + (warns.size() > 1 ? "  (+" + std::to_string(warns.size() - 1) + ")" : "");
    const std::string dir = rs_.dir, folder = rs_.folder;
    const std::string journal = util::join(paths::data(), "reslot_journal.json");
    confirm(S("Reslottear \"", "Reslot \"") + rs_.title + "\"", msg, [this, map, dir, folder, journal]() {
        rsCancel_ = false;
        rsDone_ = 0;
        rsTotal_ = 0;
        rs_.applying = true;
        rsApply_.run([this, map, dir, folder, journal]() {
            ReslotApplyRes r;
            r.folder = folder;
            reslot::Plan plan;
            r.p = reslot::makePlan(dir, map, plan);
            if (r.p == reslot::Problem::None) {
                r.items = plan.items;
                rsTotal_ = (int)plan.ops.size();
                r.p = reslot::apply(plan, journal, &rsDone_, &rsCancel_, r.err);
            }
            return r;
        });
    });
}

void App::updateReslot(const Input& in) {
    if (in.b || (!rs_.analyzing && !rs_.an.ok() && in.a)) {
        rs_.open = false;
        return;
    }
    if (rs_.analyzing || !rs_.an.ok() || rs_.src.empty()) return;
    moveSel(rs_.sel, (int)rs_.src.size(), 1, in);
    int& d = rs_.dst[(size_t)rs_.sel];
    if (in.left && d > 0) d--;
    if (in.right && d < reslot::kMaxSlot) d++;
    if (in.x) rs_.dst = rs_.src;
    if (in.a) startReslot();
}

void App::drawReslot() {
    ui::fill(0, 0, W, H, ui::Color(0, 0, 0, 200));
    const int x = 50, y = 30, w = W - 100, h = H - 60;
    theme::panel(x, y, w, h, col::accent);
    ui::text(S("Reslottear: ", "Reslot: ") + rs_.title, x + 28, y + 20, ui::BIG, col::text, w - 56);
    int cy = y + 80;
    if (rs_.analyzing) {
        ui::text(S("Analizando el mod", "Analysing the mod") + std::string((size_t)((now_ / 350) % 4), '.'), x, cy + 150, ui::NORMAL, col::text, w, 1);
        theme::hints(S("B Cancelar", "B Cancel"), x + 28, y + h - 42, w - 56, true);
        return;
    }
    if (!rs_.an.ok()) {
        ui::textWrapped(reslotProblemText(rs_.an.problem), x + 30, cy + 40, w - 60, ui::NORMAL, col::warn, 0, 300);
        theme::hints(S("A/B Cerrar", "A/B Close"), x + 28, y + h - 42, w - 56, true);
        return;
    }
    std::string fighters;
    for (const auto& f : rs_.an.perFighter) fighters += (fighters.empty() ? "" : ", ") + f.first;
    ui::text(S("Personaje(s): ", "Fighter(s): ") + fighters, x + 30, cy, ui::NORMAL, col::text, w - 60);
    cy += 30;
    ui::text(std::to_string(rs_.an.items) + S(" elementos llevan el slot en el nombre (modelos, efectos, sonidos, retratos...)", " items carry the slot in their name (models, effects, sounds, portraits...)"), x + 30, cy, ui::SMALL, col::dim, w - 60);
    cy += 34;
    ui::text(S("Elige el slot nuevo de cada uno:", "Pick the new slot for each one:"), x + 30, cy, ui::NORMAL, col::accent, w - 60);
    cy += 36;
    // un slot destino no puede repetirse
    std::vector<int> useCount(8, 0);
    for (int dv : rs_.dst)
        if (dv >= 0 && dv <= reslot::kMaxSlot) useCount[(size_t)dv]++;
    for (size_t i = 0; i < rs_.src.size(); i++) {
        const int ry = cy + (int)i * 40;
        const bool sel = (int)i == rs_.sel;
        theme::row(x + 30, ry, 560, 34, sel, col::accent);
        const bool dup = rs_.dst[i] >= 0 && rs_.dst[i] <= reslot::kMaxSlot && useCount[(size_t)rs_.dst[i]] > 1;
        const bool changed = rs_.dst[i] != rs_.src[i];
        ui::text(slotName(rs_.src[i]), x + 54, ry + 3, ui::NORMAL, sel ? col::white : col::text, 90);
        ui::text("->", x + 150, ry + 3, ui::NORMAL, col::dim, 50);
        ui::text((sel ? "<  " : "   ") + slotName(rs_.dst[i]) + (sel ? "  >" : ""), x + 215, ry + 3, ui::NORMAL, dup ? col::bad : (changed ? col::good : col::dim), 160);
        ui::text(dup ? S("repetido", "duplicate") : (changed ? S("cambia", "changes") : S("sin cambios", "unchanged")), x + 420, ry + 6, ui::SMALL, dup ? col::bad : col::dim, 160);
    }
    int wy = cy + (int)rs_.src.size() * 40 + 8;
    const auto warns = reslotWarnings();
    for (size_t i = 0; i < warns.size() && i < 3; i++) {
        ui::text(S("Aviso: ", "Warning: ") + warns[i] + S("  (usa Herramientas > conflictos)", "  (see Tools > conflicts)"), x + 30, wy, ui::SMALL, col::warn, w - 60);
        wy += 24;
    }
    if (!rs_.an.unknown.empty()) {
        std::string u = S("Se dejan sin cambiar ", "Left unchanged: ") + std::to_string(rs_.an.unknown.size()) + S(" ruta(s) que no se reconocen: ", " path(s) not recognised: ") + rs_.an.unknown[0];
        ui::text(u, x + 30, wy, ui::SMALL, col::dim, w - 60);
    }
    theme::hints(S("A Aplicar   Arr/Abj Slot   Izq/Der Nuevo   X Restablecer   B Cerrar", "A Apply   Up/Down Slot   Left/Right New   X Reset   B Close"), x + 28, y + h - 42, w - 56);
}

// ============================================================================ descargas

void App::updateDownloads(const Input& in) {
    auto jobs = dl_.jobs();
    int n = (int)jobs.size();
    if (in.y) dl_.clearFinished();
    if (n == 0) return;
    moveSel(dlSel_, n, 1, in);
    const auto& j = jobs[(size_t)dlSel_];
    int s = j->state.load();
    if (in.x) {
        if (jobActive(*j)) dl_.cancel(j->id);
        else dl_.remove(j->id);
    }
    if (in.a && (s == Job::Failed || s == Job::Cancelled)) {
        if (!j->localPath.empty()) dl_.enqueueLocal(j->localPath, j->item.name, j->targetDir);
        else dl_.enqueue(j->item, j->file, j->replaceFolder, j->targetDir);
        dl_.remove(j->id);
    }
    if (dlSel_ >= (int)dl_.jobs().size()) dlSel_ = std::max(0, (int)dl_.jobs().size() - 1);
}

void App::drawDownloads() {
    auto jobs = dl_.jobs();
    theme::subbar(HEAD, SUB, col::accent);
    ui::text(std::to_string(dl_.activeCount()) + S(" activas   -   ", " active   -   ") + std::to_string(jobs.size()) + S(" en total", " total"), 30, HEAD + 6, ui::SMALL, col::text, 800);
    uint64_t totalBps = 0;
    for (const auto& jb : jobs)
        if (jb->state.load() == Job::Downloading) totalBps += jb->prog.bps.load();
    if (totalBps > 0) ui::text(S("Velocidad: ", "Speed: ") + util::humanSize(totalBps) + "/s", W - 430, HEAD + 6, ui::SMALL, col::accent, 400, 2);
    if (jobs.empty()) {
        drawCenterMessage(S("No hay descargas", "No downloads"), S("Las descargas e instalaciones aparecerán aquí", "Downloads and installs will show up here"), col::dim);
        return;
    }
    const int rowH = 88, top = HEAD + SUB + 10, vis = (H - FOOT - top) / rowH;
    int n = (int)jobs.size();
    int first = std::max(0, std::min(dlSel_ - vis / 2, n - vis));
    for (int i = first; i < std::min(n, first + vis); i++) {
        const Job& j = *jobs[(size_t)i];
        int y = top + (i - first) * rowH;
        bool sel = i == dlSel_;
        int s = j.state.load();
        ui::Color sc = s == Job::Done ? col::good : s == Job::Failed ? col::bad : s == Job::Cancelled ? col::dim : col::accent;
        theme::row(30, y, W - 70, rowH - 10, sel, sc);
        if (!sel) ui::fill(30, y, 4, rowH - 10, sc.withAlpha(150));  // el color de la barra indica el estado
        ui::text(j.item.name, 52, y + 6, ui::NORMAL, sel ? col::white : col::text, W - 130);
        std::string sub = j.file.name + (j.file.size ? "  -  " + util::humanSize(j.file.size) : "") + (j.replaceFolder.empty() ? "" : S("  -  actualización", "  -  update"));
        ui::text(sub, 52, y + 34, ui::SMALL, col::dim, W - 130);
        ui::text(jobStateText(j), 52, y + 56, ui::SMALL, sc, W - 570);
        if (s >= Job::Downloading && s <= Job::Extracting) drawBar(W - 520, y + 58, 450, 14, j.prog.fraction(), col::accent);
    }
    ui::scrollbar(W - 14, top, vis * rowH, vis * rowH, n * rowH, first * rowH);
}

// ============================================================================ perfiles

void App::updateProfiles(const Input& in) {
    auto names = store_.profileNames();
    if (in.y) {
        askText(S("Nombre del perfil (guarda los mods activados ahora)", "Profile name (saves the currently enabled mods)"), "", 40, false, [this](const std::string& text) {
            const std::string name = util::trim(text);
            if (name.empty()) return;
            store_.saveProfile(name);
            toast(S("Perfil guardado: ", "Profile saved: ") + name, col::good);
        });
        return;
    }
    if (names.empty()) return;
    moveSel(profSel_, (int)names.size(), 1, in);
    const std::string name = names[(size_t)profSel_];
    if (in.a) {
        confirm(S("Aplicar perfil", "Apply profile"), S("Se activarán/desactivarán mods para que coincidan con el perfil \"", "Mods will be enabled/disabled to match profile \"") + name + "\".", [this, name]() {
            std::string err;
            int n = store_.applyProfile(name, err);
            toast(std::to_string(n) + S(" cambio(s). Reinicia el juego para aplicarlos.", " change(s). Restart the game to apply them."), err.empty() ? col::good : col::bad);
        });
    }
    if (in.x) {
        confirm(S("Borrar perfil", "Delete profile"), S("¿Borrar el perfil \"", "Delete profile \"") + name + "\"?", [this, name]() {
            store_.deleteProfile(name);
            toast(S("Perfil borrado", "Profile deleted"), col::dim);
            if (profSel_ >= (int)store_.profileNames().size()) profSel_ = std::max(0, (int)store_.profileNames().size() - 1);
        });
    }
}

void App::drawProfiles() {
    auto names = store_.profileNames();
    theme::subbar(HEAD, SUB, col::accent);
    ui::text(S("Ahora: ", "Now: ") + std::to_string(store_.enabledCount()) + " / " + std::to_string(store_.mods().size()) + S(" mods activados", " mods enabled"), 30, HEAD + 6, ui::SMALL, col::text, 800);
    if (names.empty()) {
        drawCenterMessage(S("No hay perfiles", "No profiles"), S("Y: guardar los mods activados ahora como perfil (p. ej. \"Online\", \"Torneo\", \"Skins\")", "Y: save the currently enabled mods as a profile (e.g. \"Online\", \"Tournament\", \"Skins\")"), col::dim);
        return;
    }
    const int rowH = 70, top = HEAD + SUB + 10, vis = (H - FOOT - top) / rowH;
    int n = (int)names.size();
    int first = std::max(0, std::min(profSel_ - vis / 2, n - vis));
    for (int i = first; i < std::min(n, first + vis); i++) {
        const auto* list = store_.profile(names[(size_t)i]);
        int y = top + (i - first) * rowH;
        bool sel = i == profSel_;
        theme::row(30, y, W - 70, rowH - 10, sel, col::accent);
        ui::text(names[(size_t)i], 52, y + 6, ui::NORMAL, sel ? col::white : col::text, 700);
        int present = 0, missing = 0;
        if (list) {
            for (const auto& f : *list) (store_.findByFolder(f) >= 0 ? present : missing)++;
        }
        std::string sub = std::to_string(list ? list->size() : 0) + S(" mods", " mods");
        if (missing) sub += "   -   " + std::to_string(missing) + S(" ya no están instalados", " no longer installed");
        ui::text(sub, 52, y + 34, ui::SMALL, missing ? col::warn : col::dim, W - 130);
    }
    ui::scrollbar(W - 14, top, vis * rowH, vis * rowH, n * rowH, first * rowH);
}

// ============================================================================ herramientas

std::string App::toolLabel(int idx, std::string& value) const {
    value.clear();
    switch (idx) {
        case 0: return S("Buscar conflictos entre mods activados", "Find conflicts between enabled mods");
        case 1: return S("Buscar actualizaciones de los mods instalados", "Check installed mods for updates");
        case 2: {
            value = std::to_string(importCount_) + S(" archivo(s)", " file(s)");
            return S("Instalar archivos de switch/smash-mod-hub/import", "Install files from switch/smash-mod-hub/import");
        }
        case 3: return S("Activar todos los mods", "Enable all mods");
        case 4: return S("Desactivar todos los mods", "Disable all mods");
        case 5: value = g_settings.english ? "English" : "Español"; return S("Idioma", "Language");
        case 6: value = std::to_string(g_settings.perPage); return S("Resultados por página", "Results per page");
        case 7: value = g_settings.verifyMd5 ? S("Sí", "Yes") : "No"; return S("Verificar MD5 de las descargas", "Verify MD5 of downloads");
        case 8: value = g_settings.keepArchives ? S("Sí", "Yes") : "No"; return S("Conservar los archivos descargados", "Keep downloaded archives");
        case 9: value = std::to_string(g_settings.gameId); return S("ID del juego en GameBanana", "GameBanana game ID");
        case 10: return S("Borrar caché de imágenes y categorías", "Clear image and category cache");
        case 11: return S("Diagnóstico y registro", "Diagnostics and log");
        case 12: value = std::to_string(g_settings.dlConns); return S("Conexiones por descarga (más = más rápido)", "Connections per download (more = faster)");
        case 13: return S("Reslottear un mod (cambiar su slot c00-c07)", "Reslot a mod (change its c00-c07 slot)");
        case 14: {
            const int m = audio::mode();
            value = !audio::available() ? S("Sin audio", "No audio") : m == audio::kChip ? S("Chiptune propio", "Built-in chiptune") : m == audio::kCustom ? S("Mis archivos (", "My files (") + std::to_string(audio::customCount()) + ")" : S("Desactivada", "Off");
            return S("Música de fondo", "Background music");
        }
        case 15: value = std::to_string(g_settings.volume) + "%"; return S("Volumen de la música (izquierda/derecha)", "Music volume (left/right)");
        case 16: value = audio::nowPlaying(); return S("Siguiente canción", "Next song");
        case 17: value = g_settings.systemKeyboard && !platform::appletMode() ? S("Del sistema", "System") : S("En pantalla", "On-screen"); return S("Teclado para escribir", "Text keyboard");
        default: return S("Salir", "Exit");
    }
}

static constexpr int kToolCount = 19;

void App::updateTools(const Input& in) {
    moveSel(toolSel_, kToolCount, 1, in);
    if (toolSel_ == 15 && (in.left || in.right)) {  // volumen: izquierda/derecha de 10 en 10
        g_settings.volume = std::max(0, std::min(100, g_settings.volume + (in.right ? 10 : -10)));
        audio::setVolume(g_settings.volume);
        g_settings.save();
        return;
    }
    if (in.a) activateTool(toolSel_);
}

void App::activateTool(int idx) {
    switch (idx) {
        case 0: startConflictScan(); break;
        case 1: startUpdateCheck(true); break;
        case 2: runImport(); break;
        case 3:
            confirm(S("Activar todos", "Enable all"), S("Se activarán todos los mods instalados.", "All installed mods will be enabled."), [this]() {
                std::string err;
                int n = store_.setAll(true, err);
                toast(std::to_string(n) + S(" mod(s) activados", " mod(s) enabled"), err.empty() ? col::good : col::bad);
            });
            break;
        case 4:
            confirm(S("Desactivar todos", "Disable all"), S("Se moverán todos los mods a la carpeta de desactivados.", "All mods will be moved to the disabled folder."), [this]() {
                std::string err;
                int n = store_.setAll(false, err);
                toast(std::to_string(n) + S(" mod(s) desactivados", " mod(s) disabled"), err.empty() ? col::good : col::bad);
            });
            break;
        case 5:
            g_settings.english = !g_settings.english;
            g_settings.save();
            break;
        case 6: {
            static const int opts[] = {12, 20, 30, 40};
            int next = opts[0];
            for (int i = 0; i < 4; i++)
                if (opts[i] == g_settings.perPage) next = opts[(i + 1) % 4];
            g_settings.perPage = next;
            g_settings.save();
            break;
        }
        case 7:
            g_settings.verifyMd5 = !g_settings.verifyMd5;
            g_settings.save();
            break;
        case 8:
            g_settings.keepArchives = !g_settings.keepArchives;
            g_settings.save();
            break;
        case 14: {
            if (!audio::available()) {
                toast(S("No se pudo iniciar el audio de la consola", "Could not start the console audio"), col::warn);
                break;
            }
            audio::rescan();
            int next = (audio::mode() + 1) % 3;
            if (next == audio::kCustom && audio::customCount() == 0) {
                toast(S("No hay archivos .wav en switch/smash-mod-hub/music", "No .wav files in switch/smash-mod-hub/music"), col::warn);
                next = audio::kOff;
            }
            g_settings.musicMode = audio::setMode(next);
            g_settings.save();
            break;
        }
        case 15:
            g_settings.volume = g_settings.volume >= 100 ? 0 : g_settings.volume + 10;
            audio::setVolume(g_settings.volume);
            g_settings.save();
            break;
        case 16: audio::next(); break;
        case 17:
            if (platform::appletMode()) {
                toast(S("Abierta desde el Álbum: el teclado del sistema puede congelar la consola, se usa el de pantalla", "Opened from the Album: the system keyboard can freeze the console, using the on-screen one"), col::warn);
                break;
            }
            g_settings.systemKeyboard = !g_settings.systemKeyboard;
            g_settings.save();
            break;
        case 9:
            askText(S("ID del juego en GameBanana (SSBU = 6498)", "GameBanana game ID (SSBU = 6498)"), std::to_string(g_settings.gameId), 8, true, [this](const std::string& text) {
                const int id = atoi(text.c_str());
                if (id <= 0) return;
                g_settings.gameId = id;
                g_settings.save();
                gb::setGameId(id);
                catId_ = 0;
                catName_.clear();
                page_ = gb::Page();
                pageLoaded_ = false;
                pageCache_.clear();
                pageOrder_.clear();
                loadPage(1);
                toast(S("ID de juego actualizado", "Game ID updated"), col::good);
            });
            break;
        case 10:
            img_.clearDisk();
            gb::clearCategoryCache();
            toast(S("Cachés de imágenes y categorías borradas", "Image and category caches cleared"), col::good);
            break;
        case 11: showDiagnostics(); break;
        case 12: {
            static const int opts[] = {1, 2, 3, 4, 6};
            int next = opts[0];
            for (int i = 0; i < 5; i++)
                if (opts[i] == g_settings.dlConns) next = opts[(i + 1) % 5];
            g_settings.dlConns = next;
            g_settings.save();
            break;
        }
        case 13:
            tab_ = TAB_INSTALLED;
            toast(S("Elige un mod y pulsa B para reslotearlo", "Pick a mod and press B to reslot it"), col::accent);
            break;
        default:
            if (dl_.activeCount() > 0) confirm(S("Salir", "Exit"), S("Hay descargas en curso. ¿Salir igualmente?", "Downloads are in progress. Exit anyway?"), [this]() { quit_ = true; });
            else quit_ = true;
    }
}

void App::runImport() {
    std::vector<std::string> files;
    for (const auto& f : util::listFiles(paths::importDir()))
        if (isArchiveName(f)) files.push_back(f);
    if (files.empty()) {
        notice(S("Importar archivos", "Import files"), S("No hay archivos en switch/smash-mod-hub/import de la tarjeta SD.\nCopia allí .zip, .7z o .rar de mods y vuelve a pulsar esta opción.", "There are no files in switch/smash-mod-hub/import on the SD card.\nCopy mod .zip, .7z or .rar files there and select this option again."));
        return;
    }
    importDirty_ = true;
    for (const auto& f : files) dl_.enqueueLocal(util::join(paths::importDir(), f), stem(f), paths::mods());
    toast(std::to_string(files.size()) + S(" archivo(s) en cola", " file(s) queued"), col::good);
    tab_ = TAB_DOWNLOADS;
}

void App::showDiagnostics() {
    util::flushLog();
    std::vector<std::string> L;
    auto yn = [](bool b) { return b ? std::string("OK") : S("NO ENCONTRADO", "NOT FOUND"); };
    const std::string title = paths::smashTitleDir();
    L.push_back(std::string("Smash Mod Hub ") + APP_VERSION_STR);
    const std::string fw = platform::firmwareVersion();
    L.push_back(std::string("Firmware: ") + (fw.empty() ? "?" : fw) + S("   (Atmosphere 1.12.0 o superior para la 23.0.0)", "   (Atmosphere 1.12.0 or newer for 23.0.0)"));
    L.push_back("");
    L.push_back("ARCropolis (libarcropolis.nro): " + yn(util::isFile(title + "/romfs/skyline/plugins/libarcropolis.nro")));
    L.push_back("Skyline (exefs/subsdk9): " + yn(util::isFile(title + "/exefs/subsdk9")));
    L.push_back(S("Carpeta de mods: ", "Mods folder: ") + paths::mods() + "  (" + std::to_string(store_.enabledCount()) + S(" activados)", " enabled)"));
    L.push_back(S("Carpeta de desactivados: ", "Disabled folder: ") + paths::modsDisabled());
    L.push_back(S("SD libre: ", "SD free: ") + util::humanSize(util::freeSpace(paths::root() + "/")));
    L.push_back(S("TLS verificado: ", "TLS verified: ") + (http::tlsVerified() ? S("sí", "yes") : S("NO (falta romfs:/cacert.pem)", "NO (romfs:/cacert.pem missing)")));
    L.push_back(S("ID de juego GameBanana: ", "GameBanana game ID: ") + std::to_string(g_settings.gameId));
    L.push_back(S("Conexiones por descarga: ", "Connections per download: ") + std::to_string(g_settings.dlConns));
    L.push_back("");
    L.push_back(S("Registro (ultimas lineas de log.txt):", "Log (last lines of log.txt):"));
    std::string text;
    if (util::readFile(paths::logFile(), text)) {
        std::vector<std::string> all;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t e = text.find('\n', pos);
            if (e == std::string::npos) e = text.size();
            all.push_back(text.substr(pos, e - pos));
            pos = e + 1;
        }
        size_t from = all.size() > 40 ? all.size() - 40 : 0;
        for (size_t i = from; i < all.size(); i++) L.push_back(util::clipUtf8(all[i], 140));
    } else {
        L.push_back(S("(vacio)", "(empty)"));
    }
    openView(S("Diagnóstico", "Diagnostics"), L);
}

void App::drawTools() {
    const int rowH = 40, top = HEAD + 10, vis = (H - FOOT - top - 8) / rowH;
    const int first = std::max(0, std::min(kToolCount - vis, toolSel_ - vis / 2));
    for (int i = first; i < kToolCount && i < first + vis; i++) {
        std::string value;
        const std::string label = toolLabel(i, value);
        const int y = top + (i - first) * rowH;
        const bool sel = i == toolSel_;
        theme::row(30, y, W - 60, rowH - 6, sel, col::accent);
        ui::text(label, 54, y + 6, ui::NORMAL, sel ? col::white : col::text, W - 430);
        if (!value.empty()) ui::text(value, W - 370, y + 6, ui::NORMAL, col::accent, 320, 2);
    }
    if (kToolCount > vis) ui::scrollbar(W - 20, top, vis * rowH - 6, vis, kToolCount, first);
}

// ============================================================================ piezas de interfaz

void App::drawBar(int x, int y, int w, int h, double frac, ui::Color c) {
    ui::fill(x, y, w, h, ui::Color(8, 9, 14));
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    const int fw = (int)(w * frac);
    if (fw > 0) {
        ui::fill(x, y, fw, h, c);
        ui::fill(x, y, fw, std::max(1, h / 3), ui::Color(255, 255, 255, 52));  // brillo superior
        if (fw > 3 && fw < w) ui::fill(x + fw - 2, y, 2, h, ui::Color(255, 255, 255, 120));  // borde de avance
    }
    ui::outline(x, y, w, h, ui::Color(255, 255, 255, 22), 1);
}

void App::drawCenterMessage(const std::string& title, const std::string& sub, ui::Color c) {
    ui::text(title, 0, H / 2 - 70, ui::BIG, c, W, 1);
    ui::text(sub, 60, H / 2 - 20, ui::NORMAL, col::dim, W - 120, 1);
}

void App::drawCard(int x, int y, int w, int h, const Card& c) {
    theme::shadow(x, y, w, h, c.selected ? 9 : 5);
    ui::fill(x, y, w, h, c.selected ? col::panelHi : col::panel);
    const int ih = w * 9 / 16;
    ui::fill(x, y, w, ih, col::placeholder);
    if (c.tex) {
        ui::image(c.tex, x, y, w, ih);
    } else if (c.noImage) {
        ui::text(S("Sin imagen", "No image"), x, y + ih / 2 - 12, ui::SMALL, col::dim, w, 1);
    } else {  // cargando: el recuadro "respira" en vez de quedarse quieto
        const int pulse = (int)(18 + 14 * std::sin((double)now_ / 260.0));
        ui::fill(x, y, w, ih, ui::Color(255, 255, 255, (uint8_t)pulse));
    }
    if (c.dim) ui::fill(x, y, w, ih, ui::Color(0, 0, 0, 150));
    ui::fill(x, y + ih - 18, w, 18, ui::Color(0, 0, 0, 46));  // sombra suave bajo la imagen
    ui::fill(x, y + ih, w, c.selected ? 4 : 2, c.selected ? col::accent : ui::Color(0, 0, 0, 90));
    int bx = x + 6;
    for (const auto& b : c.badges) {
        const int bw = ui::textWidth(b.text, ui::SMALL) + 20;
        if (bx + bw + 8 > x + w) break;
        theme::slant(bx, y + 8, bw, 24, 8, b.col);
        ui::text(b.text, bx + 8, y + 8, ui::SMALL, kInk, bw, 1);
        bx += bw + 12;
    }
    int ty = y + ih + 8;
    ui::text(c.title, x + 10, ty, ui::NORMAL, col::text, w - 20);
    ty += ui::lineHeight(ui::NORMAL);
    ui::text(c.l2, x + 10, ty, ui::SMALL, col::dim, w - 20);
    ty += ui::lineHeight(ui::SMALL);
    ui::text(c.l3, x + 10, ty, ui::SMALL, col::dim, w - 20);
    if (c.selected) theme::glow(x, y, w, h, col::accent);
}

void App::drawHeader() {
    ui::fill(0, 0, W, HEAD, col::header);
    ui::fill(0, HEAD - 2, W, 2, col::accent.withAlpha(80));
    // logotipo: bloque de color inclinado con el nombre
    theme::slant(-40, 0, 300, HEAD - 2, 22, col::accent);
    ui::text("SMASH MOD HUB", 22, 7, ui::NORMAL, col::white, 240);
    ui::text("SMASH MOD HUB", 23, 7, ui::NORMAL, col::white, 240);  // doble trazo: negrita barata (misma textura en caché)
    ui::text(std::string("v") + APP_VERSION_STR, 24, 35, ui::SMALL, ui::Color(255, 255, 255, 205));
    // pestañas: cada una mide lo que mide su texto (así "Herramientas" no se corta); si no caben, se reduce el margen
    const int x0 = 298, avail = 768;
    std::string names[TAB_COUNT];
    int tw[TAB_COUNT], sumTw = 0;
    for (int t = 0; t < TAB_COUNT; t++) {
        names[t] = tabName(t);
        tw[t] = ui::textWidth(names[t], ui::NORMAL);
        sumTw += tw[t];
    }
    const int pad = std::max(8, std::min(36, (avail - sumTw) / TAB_COUNT));
    theme::chip(x0 - 32, 20, "L");
    int x = x0;
    for (int t = 0; t < TAB_COUNT; t++) {
        const int w = tw[t] + pad;
        const bool act = t == tab_;
        if (act) {
            theme::slant(x + 2, 10, w - 6, HEAD - 22, 14, col::accent);
            ui::fill(x + 16, 10, w - 6, 2, ui::Color(255, 255, 255, 70));
        }
        ui::text(names[t], x + 4, 18, ui::NORMAL, act ? col::white : col::dim, w - 8, 1);
        x += w;
    }
    theme::chip(x + 6, 20, "R");
    theme::equalizer(W - 170, 19, 29, 26, audio::level(), now_, audio::available() && audio::mode() != audio::kOff);
    ui::text("ON " + std::to_string(store_.enabledCount()) + "/" + std::to_string(store_.mods().size()), W - 136, 18, ui::NORMAL, col::good, 116, 2);
}

void App::drawFooter(const std::string& hints) {
    ui::fill(0, H - FOOT, W, FOOT, col::header);
    ui::fill(0, H - FOOT, W, 2, col::accent.withAlpha(80));
    theme::hints(hints, 20, H - FOOT + 9, W - 300);
    if (freeSpace_) ui::text(S("SD libre: ", "SD free: ") + util::humanSize(freeSpace_), W - 270, H - FOOT + 11, ui::SMALL, col::dim, 250, 2);
}

void App::drawMain() {
    switch (tab_) {
        case TAB_BROWSE: drawBrowse(); break;
        case TAB_INSTALLED: drawInstalled(); break;
        case TAB_DOWNLOADS: drawDownloads(); break;
        case TAB_PROFILES: drawProfiles(); break;
        default: drawTools(); break;
    }
    drawHeader();
    std::string h;
    switch (tab_) {
        case TAB_BROWSE: h = S("A Ver   Y Buscar   X Orden   B Filtros   ZL/ZR Pág.   - Inicio   L/R Pestañas   + Salir", "A View   Y Search   X Sort   B Filters   ZL/ZR Page   - Home   L/R Tabs   + Exit"); break;
        case TAB_INSTALLED: h = S("A On/Off   X Borrar   Y Actualizar/Ver   B Reslot   - Buscar act.   ZL/ZR Todos off/on", "A On/Off   X Delete   Y Update/View   B Reslot   - Check updates   ZL/ZR All off/on"); break;
        case TAB_DOWNLOADS: h = S("A Reintentar   X Cancelar/Quitar   Y Limpiar terminadas", "A Retry   X Cancel/Remove   Y Clear finished"); break;
        case TAB_PROFILES: h = S("A Aplicar   X Borrar   Y Nuevo (guarda los activados)", "A Apply   X Delete   Y New (saves enabled mods)"); break;
        default: h = S("A Seleccionar / cambiar   L/R Pestañas   + Salir", "A Select / change   L/R Tabs   + Exit"); break;
    }
    drawFooter(h);
}

void App::drawToast() {
    if (now_ >= toastUntil_ || toastMsg_.empty()) return;
    const int tw = std::min(W - 80, ui::textWidth(toastMsg_, ui::NORMAL) + 56);
    // con una ventana grande abierta (categorías, reslot, detalle) los atajos ocupan la parte izquierda: el aviso va a la derecha
    const bool overlay = cp_.open || rs_.open || detailOpen_;
    const int x = overlay ? std::max(40, W - 50 - tw) : (W - tw) / 2, y = H - FOOT - 64;
    // se desvanece en los últimos 400 ms
    const uint64_t left = toastUntil_ - now_;
    const int a = left >= 400 ? 255 : (int)(255 * left / 400);
    ui::fill(x - 2, y + 4, tw + 4, 50, ui::Color(0, 0, 0, (uint8_t)(70 * a / 255)));
    ui::fill(x, y, tw, 46, ui::Color(10, 11, 18, (uint8_t)(240 * a / 255)));
    ui::fill(x, y, 8, 46, toastCol_.withAlpha((uint8_t)a));
    ui::fill(x + 8, y, tw - 8, 1, toastCol_.withAlpha((uint8_t)(a * 90 / 255)));
    ui::text(toastMsg_, x + 8, y + 9, ui::NORMAL, col::text.withAlpha((uint8_t)a), tw - 8, 1);
}

void App::drawDialog() {
    ui::fill(0, 0, W, H, ui::Color(0, 0, 0, 170));
    const int w = 760, h = 270, x = (W - w) / 2, y = (H - h) / 2;
    theme::panel(x, y, w, h, col::accent);
    ui::text(dialog_.title, x + 28, y + 26, ui::BIG, col::text, w - 56);
    ui::textWrapped(dialog_.msg, x + 28, y + 86, w - 56, ui::NORMAL, col::dim, 0, h - 156);
    theme::hints(dialog_.confirm ? S("A Aceptar   B Cancelar", "A Accept   B Cancel") : S("A Aceptar", "A OK"), x + 20, y + h - 46, w - 40, true);
}

void App::drawView() {
    ui::fill(0, 0, W, H, ui::Color(0, 0, 0, 200));
    const int x = 50, y = 30, w = W - 100, h = H - 60;
    theme::panel(x, y, w, h, col::accent);
    ui::text(view_.title, x + 24, y + 22, ui::BIG, col::text, w - 48);
    const int ay = y + 74, ah = h - 128, lh = ui::lineHeight(ui::SMALL);
    ui::clip(x + 10, ay, w - 20, ah);
    int first = std::max(0, (int)(view_.scroll / lh));
    for (int i = first; i < (int)view_.lines.size(); i++) {
        int ly = ay + i * lh - (int)view_.scroll;
        if (ly > ay + ah) break;
        ui::text(view_.lines[(size_t)i], x + 24, ly, ui::SMALL, col::text, w - 48);
    }
    ui::unclip();
    theme::hints(S("Arriba/Abajo Desplazar   B Cerrar", "Up/Down Scroll   B Close"), x + 20, y + h - 44, w - 40, true);
}

void App::drawBusyOverlay(const std::string& msg, double frac) {
    ui::fill(0, 0, W, H, ui::Color(0, 0, 0, 170));
    const int w = 700, h = 176, x = (W - w) / 2, y = (H - h) / 2;
    theme::panel(x, y, w, h, col::accent);
    ui::text(msg, x + 20, y + 30, ui::NORMAL, col::text, w - 40, 1);
    drawBar(x + 40, y + 80, w - 80, 20, frac, col::accent);
    theme::hints(S("B Cancelar", "B Cancel"), x + 20, y + 126, w - 40, true);
}

// ---- teclado en pantalla

void App::askText(const std::string& title, const std::string& initial, int maxLen, bool numeric, std::function<void(const std::string&)> done) {
    // El teclado del sistema bloquea el hilo principal hasta que se cierra y, abierta la app desde el Álbum
    // (modo applet), puede dejar la consola congelada: por eso el teclado propio es el predeterminado.
    if (g_settings.systemKeyboard && !platform::appletMode()) {
        std::string out;
        if (platform::textInput(title, initial, out, maxLen, numeric) && done) done(out);
        return;
    }
    oskDone_ = std::move(done);
    osk_.open(title, initial, maxLen, numeric);
}

void App::updateOsk(const Input& in) {
    const osk::Result r = osk_.update(in);
    if (r == osk::Result::None) return;
    auto cb = std::move(oskDone_);
    oskDone_ = nullptr;
    if (r == osk::Result::Accepted && cb) cb(osk_.text());
}

void App::drawOsk() {
    const osk::Geometry g = osk_.geometry();
    const bool num = osk_.numeric();
    ui::fill(0, 0, W, H, ui::Color(0, 0, 0, 190));
    const int pw = num ? 800 : 1080, px = (W - pw) / 2, py = 30, ph = H - FOOT - py - 6;
    theme::panel(px, py, pw, ph, col::accent);
    ui::text(osk_.title(), px + 28, py + 26, ui::BIG, col::text, pw - 56 - 130);
    ui::text(std::to_string(osk_.length()) + "/" + std::to_string(osk_.maxLen()), px + pw - 160, py + 36, ui::SMALL, col::dim, 132, 2);
    // campo de texto con cursor parpadeante
    const int fx = g.x, fy = py + 100, fw = g.w, fh = 64;
    ui::fill(fx, fy, fw, fh, col::bg);
    ui::fill(fx, fy + fh - 3, fw, 3, col::accent);
    ui::outline(fx, fy, fw, fh, ui::Color(255, 255, 255, 28), 1);
    const int ty = fy + (fh - ui::lineHeight(ui::NORMAL)) / 2 - 2;
    ui::text(osk_.text(), fx + 16, ty, ui::NORMAL, col::text, fw - 32);
    if ((now_ / 500) % 2 == 0) {
        const int cx = std::min(fx + fw - 14, fx + 16 + ui::textWidth(osk_.beforeCursor(), ui::NORMAL));
        ui::fill(cx, fy + 12, 2, fh - 24, col::accent);
    }
    // teclas
    const auto boxes = osk_.layout();
    for (const auto& b : boxes) {
        const osk::Key& k = osk_.rows()[(size_t)b.row][(size_t)b.col];
        const bool sel = b.row == osk_.selRow() && b.col == osk_.selCol();
        std::string label;
        ui::Color base = col::panelHi;
        switch (k.kind) {
            case osk::Kind::Char: label = osk_.shown(k); break;
            case osk::Kind::Shift:
                label = S("Mayús", "Shift");
                if (osk_.shift()) base = ui::Color(176, 56, 80);
                break;
            case osk::Kind::Symbols: label = osk_.symbols() ? "ABC" : S("Símbolos", "Symbols"); break;
            case osk::Kind::Space: label = S("Espacio", "Space"); break;
            case osk::Kind::Back: label = S("Borrar", "Delete"); break;
            case osk::Kind::Cancel:
                label = S("Cancelar", "Cancel");
                base = ui::Color(104, 40, 52);
                break;
            case osk::Kind::Accept:
                label = S("Aceptar", "OK");
                base = ui::Color(34, 104, 68);
                break;
        }
        ui::fill(b.x, b.y + 3, b.w, b.h, ui::Color(0, 0, 0, 110));  // borde inferior: da relieve de tecla
        ui::fill(b.x, b.y, b.w, b.h, sel ? col::accent : base);
        ui::fill(b.x, b.y, b.w, 2, ui::Color(255, 255, 255, sel ? 80 : 36));
        if (sel) ui::outline(b.x - 2, b.y - 2, b.w + 4, b.h + 4, col::white, 2);
        ui::text(label, b.x, b.y + (b.h - ui::lineHeight(ui::NORMAL)) / 2 - 1, ui::NORMAL, col::text, b.w - 6, 1);
    }
    theme::hints(num ? S("A Escribir   B Borrar   + Aceptar   - Cancelar", "A Type   B Delete   + OK   - Cancel")
                     : S("A Escribir   B Borrar   X Espacio   Y Mayús   L/R Cursor   + Aceptar   - Cancelar   (o toca la pantalla)", "A Type   B Delete   X Space   Y Shift   L/R Cursor   + OK   - Cancel   (or touch the screen)"),
                 px + 20, py + ph - 46, pw - 40, true);
}

// ============================================================================ bucle principal

void App::updateDialog(const Input& in) {
    if (in.a) {
        auto f = dialog_.onYes;
        dialog_.open = false;
        dialog_.onYes = nullptr;
        if (f) f();
    } else if (in.b && dialog_.confirm) {
        dialog_.open = false;
        dialog_.onYes = nullptr;
    }
}

void App::updateView(const Input& in) {
    const int lh = ui::lineHeight(ui::SMALL), ah = H - 60 - 120;
    float maxScroll = (float)std::max(0, (int)view_.lines.size() * lh - ah);
    if (in.up) view_.scroll -= lh * 3;
    if (in.down) view_.scroll += lh * 3;
    view_.scroll += in.stickY * 18.f;
    if (in.zl) view_.scroll -= ah;
    if (in.zr) view_.scroll += ah;
    view_.scroll = std::max(0.f, std::min(view_.scroll, maxScroll));
    if (in.b || in.a) view_.open = false;
}

void App::updateMain(const Input& in) {
    if (in.plus) {
        if (dl_.activeCount() > 0) confirm(S("Salir", "Exit"), S("Hay descargas en curso. ¿Salir igualmente?", "Downloads are in progress. Exit anyway?"), [this]() { quit_ = true; });
        else quit_ = true;
        return;
    }
    if (in.l || in.r) {
        tab_ = (tab_ + (in.l ? TAB_COUNT - 1 : 1)) % TAB_COUNT;
        importDirty_ = true;
        return;
    }
    switch (tab_) {
        case TAB_BROWSE: updateBrowse(in); break;
        case TAB_INSTALLED: updateInstalled(in); break;
        case TAB_DOWNLOADS: updateDownloads(in); break;
        case TAB_PROFILES: updateProfiles(in); break;
        default: updateTools(in); break;
    }
}

void App::frame(const Input& in) {
    now_ = sys::nowMs();
    // Al despertar la consola tras suspenderla, las conexiones guardadas están muertas: se cierran para que la
    // siguiente petición abra una nueva en vez de fallar. (El reloj de pared avanza durante la suspensión; el monotónico no.)
    const int64_t wall = (int64_t)time(nullptr);
    if (lastWall_ > 0 && lastMono_ > 0 && wall - lastWall_ - (int64_t)((now_ - lastMono_) / 1000) >= 5) {
        util::log("reanudado tras suspensión: se cierran las conexiones guardadas");
        http::dropIdle();
        lastStatus_ = 0;
    }
    lastWall_ = wall;
    lastMono_ = now_;
    platform::cpuBoost(dl_.activeCount() > 0);  // CPU a tope solo mientras se descarga/instala
    if (sys::faultCount() != lastFaults_) {
        util::log("excepciones atrapadas en hilos de fondo: %d", sys::faultCount());
        lastFaults_ = sys::faultCount();
    }
    if (now_ - lastStatus_ > 15000) refreshStatus();
    if (!autoUpdChecked_ && now_ - startedAt_ > 6000 && pageLoaded_) {
        autoUpdChecked_ = true;
        startUpdateCheck(false);
    }
    if (tab_ == TAB_TOOLS && importDirty_) refreshImportCount();
    img_.pump();
    audio::pump();
    {
        const std::string ae = audio::lastError();
        if (ae != audioErr_) {
            audioErr_ = ae;
            if (!ae.empty()) toast(S("Música: ", "Music: ") + ae, col::warn);
        }
    }
    pollAsync();
    handleFinishedJobs();
    browseScroll_.step();
    instScroll_.step();

    if (osk_.isOpen()) updateOsk(in);
    else if (dialog_.open) updateDialog(in);
    else if (view_.open) updateView(in);
    else if (confJob_.busy.load()) {
        if (in.b) confCancel_ = true;
    } else if (rs_.applying) {
        if (in.b) rsCancel_ = true;
    } else if (rs_.open) updateReslot(in);
    else if (cp_.open) updateCatPicker(in);
    else if (detailOpen_) updateDetail(in);
    else updateMain(in);

    SDL_SetRenderDrawColor(ren_, col::bg.r, col::bg.g, col::bg.b, 255);
    SDL_RenderClear(ren_);
    theme::background(now_);
    if (detailOpen_) drawDetail();
    else drawMain();
    if (confJob_.busy.load()) drawBusyOverlay(S("Buscando conflictos... ", "Scanning for conflicts... ") + std::to_string(confDone_.load()) + "/" + std::to_string(confTotal_), confTotal_ ? (double)confDone_.load() / confTotal_ : 0.0);
    if (cp_.open) drawCatPicker();
    if (rs_.open) drawReslot();
    if (rs_.applying) drawBusyOverlay(S("Reslot en curso... ", "Reslotting... ") + std::to_string(rsDone_.load()) + "/" + std::to_string(rsTotal_.load()), rsTotal_.load() ? (double)rsDone_.load() / rsTotal_.load() : 0.0);
    if (view_.open) drawView();
    if (osk_.isOpen()) drawOsk();
    drawToast();
    if (dialog_.open) drawDialog();
    ui::endFrame();
    // Diagnóstico: un fotograma muy lento se anota en el registro para poder localizar congelaciones.
    const uint64_t dt = sys::nowMs() - now_;
    if (dt > 150 && now_ - lastSlowLog_ > 4000) {
        lastSlowLog_ = now_;
        util::log("fotograma lento: %llu ms (pestaña %d, detalle %d, descargas %d)", (unsigned long long)dt, (int)tab_, detailOpen_ ? 1 : 0, dl_.activeCount());
    }
}
