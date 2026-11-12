#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "http.hpp"
#include "platform.hpp"
#include "settings.hpp"
#include "sys.hpp"
#include "util.hpp"

#ifndef APP_VERSION_STR
#define APP_VERSION_STR "1.0.0"
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

std::string jobStateText(const Job& j) {
    int s = j.state.load();
    int pct = (int)(j.prog.fraction() * 100.0);
    char b[160];
    switch (s) {
        case Job::Queued: return S("En cola", "Queued");
        case Job::Downloading:
            snprintf(b, sizeof b, "%s %d%%  (%s / %s)", T("Descargando", "Downloading"), pct, util::humanSize(j.prog.now.load()).c_str(), util::humanSize(j.prog.total.load()).c_str());
            return b;
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
    http::init("romfs:/cacert.pem");
    img_.init(r, paths::cache());
    store_.load();
    dl_.start();
    refreshStatus();
    loadPage(1);
    startUpdateCheck(false);
    if (!http::tlsVerified()) toast(S("Aviso: sin certificados TLS (cacert.pem); las conexiones no se verifican", "Warning: no TLS certificates (cacert.pem); connections are not verified"), col::warn);
    return true;
}

void App::shutdown() {
    bgCancel_ = true;
    confCancel_ = true;
    dl_.stop();
    pageJob_.join();
    detailJob_.join();
    updJob_.join();
    updPrep_.join();
    sizeJob_.join();
    confJob_.join();
    img_.shutdown();
    store_.save();
    http::shutdown();
}

void App::refreshStatus() {
    freeSpace_ = util::freeSpace(paths::root() + "/");
    lastStatus_ = sys::nowMs();
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
    gb::Page pg;
    if (pageJob_.take(pg)) {
        pageLoaded_ = true;
        if (pg.ok) {
            page_ = std::move(pg);
            pageNo_ = page_.page;
            pageErr_.clear();
            sel_ = 0;
            browseScroll_.reset();
        } else {
            pageErr_ = pg.err;
            pageNo_ = page_.page;
            if (!page_.items.empty()) toast(S("No se pudo cargar la página: ", "Could not load page: ") + pg.err, col::bad);
        }
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

void App::loadPage(int page) {
    if (pageJob_.busy.load()) return;
    pageErr_.clear();
    std::string q = query_;
    gb::Sort s = sort_;
    int p = std::max(1, page);
    int per = g_settings.perPage;
    pageJob_.run([q, s, p, per]() { return q.empty() ? gb::browse(p, s, per) : gb::search(q, p, per); });
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
    if (pageJob_.busy.load()) return;
    const auto& items = page_.items;
    if (!items.empty()) {
        moveSel(sel_, (int)items.size(), COLS, in);
        ensureVisible(sel_, COLS, browseScroll_, H - FOOT - (HEAD + SUB) - 10);
        if (in.a) openDetail(items[(size_t)sel_]);
    } else if (in.a) {
        loadPage(pageNo_);
    }
    if (in.y) {
        std::string q;
        if (platform::textInput(S("Buscar mods", "Search mods"), query_, q, 50) && util::trim(q) != query_) {
            query_ = util::trim(q);
            loadPage(1);
        }
    }
    if (in.x) {
        if (!query_.empty()) {
            toast(S("El orden no se aplica a las búsquedas", "Sorting does not apply to searches"), col::dim);
        } else {
            sort_ = sort_ == gb::Sort::Default ? gb::Sort::New : sort_ == gb::Sort::New ? gb::Sort::Updated : gb::Sort::Default;
            loadPage(1);
        }
    }
    if (in.minus) {
        query_.clear();
        loadPage(1);
    }
    if (in.zr && !page_.complete && !items.empty()) loadPage(pageNo_ + 1);
    if (in.zl && pageNo_ > 1) loadPage(pageNo_ - 1);
}

void App::drawBrowse() {
    const int areaY = HEAD + SUB, areaH = H - FOOT - areaY;
    ui::fill(0, HEAD, W, SUB, col::panel);
    std::string left = query_.empty() ? S("Orden: ", "Sort: ") + sortName(sort_) : S("Búsqueda: ", "Search: ") + "\"" + query_ + "\"";
    ui::text(left, 30, HEAD + 6, ui::SMALL, col::text, 700);
    std::string right = S("Página ", "Page ") + std::to_string(pageNo_) + (page_.items.empty() ? "" : "   -   " + std::to_string(page_.items.size()) + S(" mods", " mods"));
    ui::text(right, W - 530, HEAD + 6, ui::SMALL, col::dim, 500, 2);

    bool busy = pageJob_.busy.load();
    if (page_.items.empty()) {
        if (busy || !pageLoaded_) {
            drawCenterMessage(S("Cargando", "Loading") + std::string((size_t)((now_ / 350) % 4), '.'), S("Conectando con GameBanana", "Connecting to GameBanana"), col::text);
        } else if (!pageErr_.empty()) {
            drawCenterMessage(S("No se pudo conectar", "Could not connect"), pageErr_ + "   -   " + S("A: reintentar", "A: retry"), col::bad);
        } else {
            std::string sub = page_.dropped > 0 ? S("Se descartaron resultados de otro juego: revisa el ID de juego en Herramientas", "Results from another game were dropped: check the game ID in Tools") : S("Prueba otra búsqueda o pulsa - para volver al inicio", "Try another search or press - to go back");
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
    if (busy) ui::text(S("Cargando", "Loading") + std::string((size_t)((now_ / 350) % 4), '.'), W - 230, HEAD + 6, ui::SMALL, col::accent, 200, 2);
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
    ui::fill(0, 0, W, H, col::bg);
    ui::fill(0, 0, W, HEAD, col::header);
    ui::text(det_.item.name, 30, 12, ui::BIG, col::text, W - 60);

    // --- imagen
    const int lx = 40, ly = HEAD + 16, lw = 640, lh = 360;
    ui::fill(lx, ly, lw, lh, col::placeholder);
    std::string url = det_.item.galleryUrl((size_t)detImg_);
    SDL_Texture* t = url.empty() ? nullptr : img_.get(url, true);
    if (t) ui::imageContain(t, lx, ly, lw, lh);
    else ui::text(url.empty() ? S("Sin imagen", "No image") : S("Cargando imagen...", "Loading image..."), lx, ly + lh / 2 - 12, ui::NORMAL, col::dim, lw, 1);
    if (det_.item.images.size() > 1) {
        std::string n = "< " + std::to_string(detImg_ + 1) + " / " + std::to_string(det_.item.images.size()) + " >";
        int tw = ui::textWidth(n, ui::SMALL) + 20;
        ui::fill(lx + lw - tw - 10, ly + 10, tw, 28, col::bg.withAlpha(200));
        ui::text(n, lx + lw - tw, ly + 12, ui::SMALL, col::text);
    }

    // --- descripción (se desplaza con el stick derecho)
    const int dy = ly + lh + 12, dh = H - FOOT - dy;
    ui::fill(lx, dy, lw, dh, col::panel);
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
    ui::text(S("Archivos", "Files"), rx, y, ui::NORMAL, col::accent);
    y += 32;
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
            ui::fill(rx, ry, rw, rowH - 4, sel ? col::panelHi : col::panel);
            if (sel) ui::outline(rx, ry, rw, rowH - 4, col::accent, 2);
            bool installed = mi >= 0 && store_.mods()[(size_t)mi].meta.fileId == f.id;
            ui::text(f.name, rx + 10, ry + 2, ui::NORMAL, col::text, rw - 20 - (installed ? 120 : 0));
            std::string sub = util::humanSize(f.size);
            if (f.added) sub += "  -  " + util::formatDate(f.added);
            if (!f.desc.empty()) sub += "  -  " + f.desc;
            ui::text(sub, rx + 10, ry + 28, ui::SMALL, col::dim, rw - 20);
            if (installed) ui::text(S("INSTALADO", "INSTALLED"), rx + rw - 120, ry + 4, ui::SMALL, col::good, 110, 2);
        }
        if (n > maxRows) ui::text(std::to_string(detFile_ + 1) + " / " + std::to_string(n), rx, y + maxRows * rowH, ui::SMALL, col::dim, rw, 2);

        // botón de descarga
        const gb::File& f = det_.files[(size_t)detFile_];
        ui::fill(rx, H - FOOT - 70, rw, 56, col::accent);
        ui::text("A   " + (mi >= 0 ? S("Reemplazar", "Replace") : S("Descargar", "Download")) + "  (" + util::humanSize(f.size) + ")", rx, H - FOOT - 70 + 14, ui::NORMAL, col::white, rw, 1);
    }
    drawFooter(S("A Descargar   B Volver   Izq/Der Capturas   Arriba/Abajo Archivo   Stick der. Descripción", "A Download   B Back   Left/Right Screenshots   Up/Down File   R stick Description"));
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
    if (in.y) {
        if (m.hasUpdate()) startModUpdate((size_t)instSel_);
        else if (m.meta.gbId) openDetail(itemFromMod(m));
        else toast(S("Este mod no viene de GameBanana", "This mod did not come from GameBanana"), col::dim);
    }
}

void App::drawInstalled() {
    const int areaY = HEAD + SUB, areaH = H - FOOT - areaY;
    ui::fill(0, HEAD, W, SUB, col::panel);
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
    ui::fill(0, HEAD, W, SUB, col::panel);
    ui::text(std::to_string(dl_.activeCount()) + S(" activas   -   ", " active   -   ") + std::to_string(jobs.size()) + S(" en total", " total"), 30, HEAD + 6, ui::SMALL, col::text, 800);
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
        ui::fill(30, y, W - 60, rowH - 8, sel ? col::panelHi : col::panel);
        if (sel) ui::outline(30, y, W - 60, rowH - 8, col::accent, 2);
        int s = j.state.load();
        ui::Color sc = s == Job::Done ? col::good : s == Job::Failed ? col::bad : s == Job::Cancelled ? col::dim : col::accent;
        ui::text(j.item.name, 48, y + 6, ui::NORMAL, col::text, W - 120);
        std::string sub = j.file.name + (j.file.size ? "  -  " + util::humanSize(j.file.size) : "") + (j.replaceFolder.empty() ? "" : S("  -  actualización", "  -  update"));
        ui::text(sub, 48, y + 34, ui::SMALL, col::dim, W - 120);
        ui::text(jobStateText(j), 48, y + 56, ui::SMALL, sc, W - 560);
        if (s >= Job::Downloading && s <= Job::Extracting) drawBar(W - 520, y + 58, 450, 14, j.prog.fraction(), col::accent);
    }
}

// ============================================================================ perfiles

void App::updateProfiles(const Input& in) {
    auto names = store_.profileNames();
    if (in.y) {
        std::string name;
        if (platform::textInput(S("Nombre del perfil (guarda los mods activados ahora)", "Profile name (saves the currently enabled mods)"), "", name, 40)) {
            name = util::trim(name);
            if (!name.empty()) {
                store_.saveProfile(name);
                toast(S("Perfil guardado: ", "Profile saved: ") + name, col::good);
            }
        }
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
    ui::fill(0, HEAD, W, SUB, col::panel);
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
        ui::fill(30, y, W - 60, rowH - 8, sel ? col::panelHi : col::panel);
        if (sel) ui::outline(30, y, W - 60, rowH - 8, col::accent, 2);
        ui::text(names[(size_t)i], 48, y + 6, ui::NORMAL, col::text, 700);
        int present = 0, missing = 0;
        if (list) {
            for (const auto& f : *list) (store_.findByFolder(f) >= 0 ? present : missing)++;
        }
        std::string sub = std::to_string(list ? list->size() : 0) + S(" mods", " mods");
        if (missing) sub += "   -   " + std::to_string(missing) + S(" ya no están instalados", " no longer installed");
        ui::text(sub, 48, y + 34, ui::SMALL, missing ? col::warn : col::dim, W - 120);
    }
}

// ============================================================================ herramientas

std::string App::toolLabel(int idx, std::string& value) const {
    value.clear();
    switch (idx) {
        case 0: return S("Buscar conflictos entre mods activados", "Find conflicts between enabled mods");
        case 1: return S("Buscar actualizaciones de los mods instalados", "Check installed mods for updates");
        case 2: {
            int n = 0;
            for (const auto& f : util::listFiles(paths::importDir())) n += isArchiveName(f) ? 1 : 0;
            value = std::to_string(n) + S(" archivo(s)", " file(s)");
            return S("Instalar archivos de switch/smash-mod-hub/import", "Install files from switch/smash-mod-hub/import");
        }
        case 3: return S("Activar todos los mods", "Enable all mods");
        case 4: return S("Desactivar todos los mods", "Disable all mods");
        case 5: value = g_settings.english ? "English" : "Español"; return S("Idioma", "Language");
        case 6: value = std::to_string(g_settings.perPage); return S("Resultados por página", "Results per page");
        case 7: value = g_settings.verifyMd5 ? S("Sí", "Yes") : "No"; return S("Verificar MD5 de las descargas", "Verify MD5 of downloads");
        case 8: value = g_settings.keepArchives ? S("Sí", "Yes") : "No"; return S("Conservar los archivos descargados", "Keep downloaded archives");
        case 9: value = std::to_string(g_settings.gameId); return S("ID del juego en GameBanana", "GameBanana game ID");
        case 10: return S("Borrar caché de imágenes", "Clear image cache");
        case 11: return S("Diagnóstico y registro", "Diagnostics and log");
        default: return S("Salir", "Exit");
    }
}

static constexpr int kToolCount = 13;

void App::updateTools(const Input& in) {
    moveSel(toolSel_, kToolCount, 1, in);
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
        case 9: {
            std::string s;
            if (platform::textInput(S("ID del juego en GameBanana (SSBU = 6498)", "GameBanana game ID (SSBU = 6498)"), std::to_string(g_settings.gameId), s, 8, true)) {
                int id = atoi(s.c_str());
                if (id > 0) {
                    g_settings.gameId = id;
                    g_settings.save();
                    gb::setGameId(id);
                    page_ = gb::Page();
                    pageLoaded_ = false;
                    loadPage(1);
                    toast(S("ID de juego actualizado", "Game ID updated"), col::good);
                }
            }
            break;
        }
        case 10:
            img_.clearDisk();
            toast(S("Caché de imágenes borrada", "Image cache cleared"), col::good);
            break;
        case 11: showDiagnostics(); break;
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
    for (const auto& f : files) dl_.enqueueLocal(util::join(paths::importDir(), f), stem(f), paths::mods());
    toast(std::to_string(files.size()) + S(" archivo(s) en cola", " file(s) queued"), col::good);
    tab_ = TAB_DOWNLOADS;
}

void App::showDiagnostics() {
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
    const int rowH = 44, top = HEAD + 10;
    for (int i = 0; i < kToolCount; i++) {
        std::string value;
        std::string label = toolLabel(i, value);
        int y = top + i * rowH;
        bool sel = i == toolSel_;
        ui::fill(30, y, W - 60, rowH - 6, sel ? col::panelHi : col::panel);
        if (sel) ui::outline(30, y, W - 60, rowH - 6, col::accent, 2);
        ui::text(label, 48, y + 5, ui::NORMAL, col::text, W - 420);
        if (!value.empty()) ui::text(value, W - 360, y + 5, ui::NORMAL, col::accent, 310, 2);
    }
}

// ============================================================================ piezas de interfaz

void App::drawBar(int x, int y, int w, int h, double frac, ui::Color c) {
    ui::fill(x, y, w, h, col::bg);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    ui::fill(x, y, (int)(w * frac), h, c);
}

void App::drawCenterMessage(const std::string& title, const std::string& sub, ui::Color c) {
    ui::text(title, 0, H / 2 - 70, ui::BIG, c, W, 1);
    ui::text(sub, 60, H / 2 - 20, ui::NORMAL, col::dim, W - 120, 1);
}

void App::drawCard(int x, int y, int w, int h, const Card& c) {
    ui::fill(x, y, w, h, c.selected ? col::panelHi : col::panel);
    const int ih = w * 9 / 16;
    ui::fill(x, y, w, ih, col::placeholder);
    if (c.tex) ui::image(c.tex, x, y, w, ih);
    else ui::text(c.noImage ? S("Sin imagen", "No image") : "...", x, y + ih / 2 - 12, ui::SMALL, col::dim, w, 1);
    if (c.dim) ui::fill(x, y, w, ih, ui::Color(0, 0, 0, 150));
    int bx = x + 8;
    for (const auto& b : c.badges) {
        int bw = ui::textWidth(b.text, ui::SMALL) + 16;
        ui::fill(bx, y + 8, bw, 24, b.col);
        ui::text(b.text, bx + 8, y + 8, ui::SMALL, kInk);
        bx += bw + 6;
    }
    int ty = y + ih + 6;
    ui::text(c.title, x + 10, ty, ui::NORMAL, col::text, w - 20);
    ty += ui::lineHeight(ui::NORMAL);
    ui::text(c.l2, x + 10, ty, ui::SMALL, col::dim, w - 20);
    ty += ui::lineHeight(ui::SMALL);
    ui::text(c.l3, x + 10, ty, ui::SMALL, col::dim, w - 20);
    if (c.selected) ui::outline(x - 3, y - 3, w + 6, h + 6, col::accent, 3);
}

void App::drawHeader() {
    ui::fill(0, 0, W, HEAD, col::header);
    ui::text("SMASH MOD HUB", 28, 14, ui::BIG, col::accent);
    const int tabW = 150, x0 = 300;
    ui::text("L", x0 - 30, 18, ui::NORMAL, col::dim);
    for (int t = 0; t < TAB_COUNT; t++) {
        int x = x0 + t * tabW;
        bool act = t == tab_;
        ui::text(tabName(t), x, 18, ui::NORMAL, act ? col::text : col::dim, tabW - 6, 1);
        if (act) ui::fill(x + 8, HEAD - 6, tabW - 22, 4, col::accent);
    }
    ui::text("R", x0 + TAB_COUNT * tabW + 8, 18, ui::NORMAL, col::dim);
    ui::text("ON " + std::to_string(store_.enabledCount()) + "/" + std::to_string(store_.mods().size()), W - 150, 18, ui::NORMAL, col::good, 130, 2);
}

void App::drawFooter(const std::string& hints) {
    ui::fill(0, H - FOOT, W, FOOT, col::header);
    ui::text(hints, 24, H - FOOT + 11, ui::SMALL, col::dim, W - 300);
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
        case TAB_BROWSE: h = S("A Ver   Y Buscar   X Orden   ZL/ZR Página   - Inicio   L/R Pestañas   + Salir", "A View   Y Search   X Sort   ZL/ZR Page   - Home   L/R Tabs   + Exit"); break;
        case TAB_INSTALLED: h = S("A Activar/Desactivar   X Borrar   Y Actualizar/Ver   - Buscar act.   ZL/ZR Todos off/on", "A Enable/Disable   X Delete   Y Update/View   - Check updates   ZL/ZR All off/on"); break;
        case TAB_DOWNLOADS: h = S("A Reintentar   X Cancelar/Quitar   Y Limpiar terminadas", "A Retry   X Cancel/Remove   Y Clear finished"); break;
        case TAB_PROFILES: h = S("A Aplicar   X Borrar   Y Nuevo (guarda los activados)", "A Apply   X Delete   Y New (saves enabled mods)"); break;
        default: h = S("A Seleccionar / cambiar   L/R Pestañas   + Salir", "A Select / change   L/R Tabs   + Exit"); break;
    }
    drawFooter(h);
}

void App::drawToast() {
    if (now_ >= toastUntil_ || toastMsg_.empty()) return;
    int tw = std::min(W - 80, ui::textWidth(toastMsg_, ui::NORMAL) + 48);
    int x = (W - tw) / 2, y = H - FOOT - 62;
    ui::fill(x, y, tw, 46, ui::Color(8, 9, 14, 235));
    ui::outline(x, y, tw, 46, toastCol_, 2);
    ui::text(toastMsg_, x, y + 8, ui::NORMAL, toastCol_, tw, 1);
}

void App::drawDialog() {
    ui::fill(0, 0, W, H, ui::Color(0, 0, 0, 170));
    const int w = 760, h = 270, x = (W - w) / 2, y = (H - h) / 2;
    ui::fill(x, y, w, h, col::panel);
    ui::outline(x, y, w, h, col::accent, 3);
    ui::text(dialog_.title, x + 28, y + 20, ui::BIG, col::text, w - 56);
    ui::textWrapped(dialog_.msg, x + 28, y + 80, w - 56, ui::NORMAL, col::dim, 0, h - 150);
    ui::text(dialog_.confirm ? S("A  Aceptar        B  Cancelar", "A  Accept        B  Cancel") : S("A  Aceptar", "A  OK"), x, y + h - 48, ui::NORMAL, col::accent, w, 1);
}

void App::drawView() {
    ui::fill(0, 0, W, H, ui::Color(0, 0, 0, 200));
    const int x = 50, y = 30, w = W - 100, h = H - 60;
    ui::fill(x, y, w, h, col::panel);
    ui::outline(x, y, w, h, col::accent, 3);
    ui::text(view_.title, x + 24, y + 14, ui::BIG, col::text, w - 48);
    const int ay = y + 70, ah = h - 120, lh = ui::lineHeight(ui::SMALL);
    ui::clip(x + 10, ay, w - 20, ah);
    int first = std::max(0, (int)(view_.scroll / lh));
    for (int i = first; i < (int)view_.lines.size(); i++) {
        int ly = ay + i * lh - (int)view_.scroll;
        if (ly > ay + ah) break;
        ui::text(view_.lines[(size_t)i], x + 24, ly, ui::SMALL, col::text, w - 48);
    }
    ui::unclip();
    ui::text(S("Arriba/Abajo o stick: desplazar        B: cerrar", "Up/Down or stick: scroll        B: close"), x, y + h - 40, ui::SMALL, col::dim, w, 1);
}

void App::drawBusyOverlay(const std::string& msg, double frac) {
    ui::fill(0, 0, W, H, ui::Color(0, 0, 0, 170));
    const int w = 700, h = 170, x = (W - w) / 2, y = (H - h) / 2;
    ui::fill(x, y, w, h, col::panel);
    ui::outline(x, y, w, h, col::accent, 3);
    ui::text(msg, x, y + 26, ui::NORMAL, col::text, w, 1);
    drawBar(x + 40, y + 80, w - 80, 20, frac, col::accent);
    ui::text(S("B  Cancelar", "B  Cancel"), x, y + 120, ui::SMALL, col::dim, w, 1);
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
    if (now_ - lastStatus_ > 5000) refreshStatus();
    img_.pump();
    pollAsync();
    handleFinishedJobs();
    browseScroll_.step();
    instScroll_.step();

    if (dialog_.open) updateDialog(in);
    else if (view_.open) updateView(in);
    else if (confJob_.busy.load()) {
        if (in.b) confCancel_ = true;
    } else if (detailOpen_) updateDetail(in);
    else updateMain(in);

    SDL_SetRenderDrawColor(ren_, col::bg.r, col::bg.g, col::bg.b, 255);
    SDL_RenderClear(ren_);
    if (detailOpen_) drawDetail();
    else drawMain();
    if (confJob_.busy.load()) drawBusyOverlay(S("Buscando conflictos... ", "Scanning for conflicts... ") + std::to_string(confDone_.load()) + "/" + std::to_string(confTotal_), confTotal_ ? (double)confDone_.load() / confTotal_ : 0.0);
    if (view_.open) drawView();
    drawToast();
    if (dialog_.open) drawDialog();
}
