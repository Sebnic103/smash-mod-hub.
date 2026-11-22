#include "reslot.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>

#include "json.hpp"
#include "util.hpp"

namespace reslot {

int testFailAt = -1;
int testCrashAt = -1;

namespace {

constexpr int kMaxDepth = 6;
constexpr size_t kMaxHits = 20000;  // tope de seguridad (un mod real tiene unas decenas)

struct Entry {
    std::string name;
    bool isDir = false;
};

// Lista una carpeta sin hacer "stat" por entrada cuando el sistema ya dice qué es cada cosa.
bool listDir(const std::string& path, std::vector<Entry>& out) {
    out.clear();
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    while (struct dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        Entry en;
        en.name = e->d_name;
#if defined(DT_DIR) && defined(DT_REG)
        if (e->d_type == DT_DIR) en.isDir = true;
        else if (e->d_type == DT_REG) en.isDir = false;
        else en.isDir = util::isDir(util::join(path, en.name));
#else
        en.isDir = util::isDir(util::join(path, en.name));
#endif
        out.push_back(std::move(en));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return true;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlnum(char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// "c05" / "C05" exacto
bool parseSlotDir(const std::string& n, int& slot, size_t& pos) {
    if (n.size() != 3 || (n[0] != 'c' && n[0] != 'C') || !isDigit(n[1]) || !isDigit(n[2])) return false;
    slot = (n[1] - '0') * 10 + (n[2] - '0');
    pos = 1;
    return true;
}

// "..._c05" seguido de fin, "." o "_"  (ef_mario_c05.eff, trail_c05, vc_mario_c05_cheer.nus3audio)
bool parseSuffixSlot(const std::string& n, int& slot, size_t& pos, size_t& stemEnd) {
    for (size_t i = 1; i + 3 <= n.size(); i++) {
        if (n[i - 1] != '_' || (n[i] != 'c' && n[i] != 'C') || !isDigit(n[i + 1]) || !isDigit(n[i + 2])) continue;
        if (i + 3 < n.size() && n[i + 3] != '.' && n[i + 3] != '_') continue;
        slot = (n[i + 1] - '0') * 10 + (n[i + 2] - '0');
        pos = i + 1;
        stemEnd = i - 1;  // índice del '_' previo
        return true;
    }
    return false;
}

// chara_<n>_<personaje>_<NN>.bntx
bool parseUiName(const std::string& n, int& slot, size_t& pos, std::string& fighter) {
    const std::string l = util::lower(n);
    if (!util::endsWith(l, ".bntx") || !util::startsWith(l, "chara_")) return false;
    const size_t dot = n.size() - 5;
    if (dot < 8 || !isDigit(n[dot - 1]) || !isDigit(n[dot - 2]) || n[dot - 3] != '_') return false;
    size_t p = 6;
    size_t d0 = p;
    while (p < n.size() && isDigit(n[p])) p++;
    if (p == d0 || p >= n.size() || n[p] != '_') return false;
    const size_t fStart = p + 1, fEnd = dot - 3;
    if (fEnd <= fStart) return false;
    fighter = n.substr(fStart, fEnd - fStart);
    slot = (n[dot - 2] - '0') * 10 + (n[dot - 1] - '0');
    pos = dot - 2;
    return true;
}

struct Hit {
    std::string parent, name;
    bool isDir = false;
    int slot = 0;
    size_t pos = 0;
    std::string fighter;
};

struct Scan {
    std::vector<Hit> hits;
    std::vector<std::string> unknown;
    bool cancelled = false;
    bool truncated = false;
};

struct Walker {
    Scan& sc;
    const std::atomic<bool>* cancel;

    bool stop() {
        if (cancel && cancel->load()) sc.cancelled = true;
        if (sc.hits.size() > kMaxHits) sc.truncated = true;
        return sc.cancelled || sc.truncated;
    }
    void add(const std::string& parent, const Entry& e, int slot, size_t pos, const std::string& fighter) {
        Hit h;
        h.parent = parent;
        h.name = e.name;
        h.isDir = e.isDir;
        h.slot = slot;
        h.pos = pos;
        h.fighter = fighter;
        sc.hits.push_back(std::move(h));
    }
    void note(const std::string& rel) {
        if (sc.unknown.size() < 5) sc.unknown.push_back(rel);
    }

    // fighter/<x>/model/** y motion/**: carpetas cNN (no se entra en ellas: ahí solo hay archivos del modelo)
    void slotTree(const std::string& dir, const std::string& fighter, int depth) {
        std::vector<Entry> es;
        if (depth > kMaxDepth || !listDir(dir, es) || stop()) return;
        for (const auto& e : es) {
            if (!e.isDir) continue;
            int slot;
            size_t pos;
            if (parseSlotDir(e.name, slot, pos)) {
                add(dir, e, slot, pos, fighter);
                continue;
            }
            std::string sub = fighter;
            if (fighter == "kirby" && util::startsWith(util::lower(e.name), "copy_")) {  // copy_<dueño>_cap: el slot es el del dueño
                const size_t us = e.name.find('_', 5);
                if (us != std::string::npos && us > 5) sub = util::lower(e.name.substr(5, us - 5));
            }
            slotTree(util::join(dir, e.name), sub, depth + 1);
        }
    }

    // Lo que hay dentro de fighter/<x>/ que no es model/motion: solo para avisar de cosas con pinta de slot
    void unknownTree(const std::string& dir, const std::string& rel, int depth) {
        std::vector<Entry> es;
        if (depth > 3 || !listDir(dir, es) || stop()) return;
        for (const auto& e : es) {
            int slot;
            size_t pos, stem;
            if (parseSlotDir(e.name, slot, pos) || parseSuffixSlot(e.name, slot, pos, stem)) note(rel + "/" + e.name);
            if (e.isDir) unknownTree(util::join(dir, e.name), rel + "/" + e.name, depth + 1);
        }
    }

    // effect/fighter/<x>/**: ef_<x>_cNN.eff, trail_cNN, <efecto>_cNN (las "c00" internas se quedan como están)
    void effectTree(const std::string& dir, const std::string& fighter, int depth) {
        std::vector<Entry> es;
        if (depth > kMaxDepth || !listDir(dir, es) || stop()) return;
        for (const auto& e : es) {
            int slot;
            size_t pos, stem;
            if (parseSuffixSlot(e.name, slot, pos, stem)) {
                add(dir, e, slot, pos, fighter);
                continue;  // si es carpeta, no se entra
            }
            if (e.isDir) effectTree(util::join(dir, e.name), fighter, depth + 1);
        }
    }

    void uiTree(const std::string& dir, int depth) {
        std::vector<Entry> es;
        if (depth > 2 || !listDir(dir, es) || stop()) return;
        for (const auto& e : es) {
            if (e.isDir) {
                uiTree(util::join(dir, e.name), depth + 1);
                continue;
            }
            int slot;
            size_t pos;
            std::string fighter;
            if (parseUiName(e.name, slot, pos, fighter)) add(dir, e, slot, pos, util::lower(fighter));
        }
    }

    void soundDir(const std::string& dir, const std::string& rel) {
        std::vector<Entry> es;
        if (!listDir(dir, es) || stop()) return;
        for (const auto& e : es) {
            if (e.isDir) continue;
            int slot;
            size_t pos, stem;
            if (!parseSuffixSlot(e.name, slot, pos, stem)) continue;
            const std::string l = util::lower(e.name);
            if ((util::startsWith(l, "se_") || util::startsWith(l, "vc_")) && stem > 3) add(dir, e, slot, pos, util::lower(e.name.substr(3, stem - 3)));
            else note(rel + "/" + e.name);
        }
    }

    void run(const std::string& root) {
        std::vector<Entry> es;
        // fighter/
        const std::string fdir = util::join(root, "fighter");
        if (listDir(fdir, es)) {
            const std::vector<Entry> fighters = es;
            for (const auto& f : fighters) {
                if (!f.isDir || stop()) continue;
                const std::string fpath = util::join(fdir, f.name);
                std::vector<Entry> kids;
                if (!listDir(fpath, kids)) continue;
                for (const auto& k : kids) {
                    if (!k.isDir) continue;
                    const std::string kl = util::lower(k.name);
                    if (kl == "model" || kl == "motion") slotTree(util::join(fpath, k.name), util::lower(f.name), 0);
                    else unknownTree(util::join(fpath, k.name), "fighter/" + f.name + "/" + k.name, 0);
                }
            }
        }
        // camera/fighter/<x>/cNN
        const std::string cdir = util::join(root, "camera/fighter");
        if (listDir(cdir, es)) {
            const std::vector<Entry> fighters = es;
            for (const auto& f : fighters) {
                if (!f.isDir || stop()) continue;
                std::vector<Entry> kids;
                if (!listDir(util::join(cdir, f.name), kids)) continue;
                for (const auto& k : kids) {
                    int slot;
                    size_t pos;
                    if (k.isDir && parseSlotDir(k.name, slot, pos)) add(util::join(cdir, f.name), k, slot, pos, util::lower(f.name));
                }
            }
        }
        // effect/fighter/<x>/**
        const std::string edir = util::join(root, "effect/fighter");
        if (listDir(edir, es)) {
            const std::vector<Entry> fighters = es;
            for (const auto& f : fighters)
                if (f.isDir && !stop()) effectTree(util::join(edir, f.name), util::lower(f.name), 0);
        }
        // sound/bank/fighter y fighter_voice
        soundDir(util::join(root, "sound/bank/fighter"), "sound/bank/fighter");
        soundDir(util::join(root, "sound/bank/fighter_voice"), "sound/bank/fighter_voice");
        // ui/replace/chara y ui/replace_patch/chara
        uiTree(util::join(root, "ui/replace/chara"), 0);
        uiTree(util::join(root, "ui/replace_patch/chara"), 0);
    }
};

Analysis analyzeScan(const std::string& modDir, Scan& sc, const std::atomic<bool>* cancel) {
    Analysis a;
    if (!util::isDir(modDir)) {
        a.problem = Problem::NotFound;
        return a;
    }
    Walker w{sc, cancel};
    w.run(modDir);
    if (sc.cancelled) {
        a.problem = Problem::Cancelled;
        return a;
    }
    std::set<int> all;
    std::map<std::string, std::set<int>> per;
    for (const auto& h : sc.hits) {
        all.insert(h.slot);
        per[h.fighter].insert(h.slot);
    }
    for (const auto& kv : per) a.perFighter[kv.first] = std::vector<int>(kv.second.begin(), kv.second.end());
    a.slots.assign(all.begin(), all.end());
    a.items = (int)sc.hits.size();
    a.unknown = sc.unknown;
    const std::string cfg = util::join(modDir, "config.json");
    std::string text;
    if (util::isFile(cfg) && util::readFile(cfg, text) && configMentionsSlots(text)) a.problem = Problem::HasConfig;
    else if (!all.empty() && *all.rbegin() > kMaxSlot) a.problem = Problem::ExtraSlots;
    else if (all.empty() || sc.truncated) a.problem = Problem::NoSlots;
    return a;
}

std::string twoDigits(int v) {
    char b[8];
    snprintf(b, sizeof b, "%02d", v);
    return b;
}

int depthOf(const std::string& p) { return (int)std::count(p.begin(), p.end(), '/'); }

}  // namespace

// ------------------------------------------------------------------ API
bool configMentionsSlots(const std::string& t) {
    for (size_t i = 0; i + 2 < t.size(); i++) {
        if ((t[i] == 'c' || t[i] == 'C') && isDigit(t[i + 1]) && isDigit(t[i + 2])) {
            const bool before = i == 0 || !isAlnum(t[i - 1]);
            const bool after = i + 3 >= t.size() || (!isAlnum(t[i + 3]));
            if (before && after) return true;
        }
        if (t[i] == '_' && isDigit(t[i + 1]) && i + 7 < t.size() && isDigit(t[i + 2]) && t[i + 3] == '.' && (t[i + 4] == 'b' || t[i + 4] == 'B') && (t[i + 5] == 'n' || t[i + 5] == 'N')) return true;
    }
    return false;
}

std::string renamedWithSlot(const std::string& name, int newSlot) {
    if (newSlot < 0 || newSlot > 99) return "";
    int slot;
    size_t pos, stem;
    std::string fighter;
    std::string out = name;
    if (parseSlotDir(name, slot, pos) || parseUiName(name, slot, pos, fighter) || parseSuffixSlot(name, slot, pos, stem)) {
        out.replace(pos, 2, twoDigits(newSlot));
        return out;
    }
    return "";
}

Analysis analyze(const std::string& modDir, const std::atomic<bool>* cancel) {
    Scan sc;
    return analyzeScan(modDir, sc, cancel);
}

std::map<std::string, std::vector<int>> fighterSlots(const std::string& modDir) {
    Scan sc;
    Analysis a = analyzeScan(modDir, sc, nullptr);
    return a.perFighter;
}

Problem makePlan(const std::string& modDir, const SlotMap& map, Plan& plan) {
    plan = Plan();
    Scan sc;
    Analysis a = analyzeScan(modDir, sc, nullptr);
    if (!a.ok()) return a.problem;
    auto eff = [&](int s) { return map.count(s) ? map.at(s) : s; };
    std::set<int> targets;
    for (int s : a.slots) {
        const int t = eff(s);
        if (t < 0 || t > kMaxSlot || !targets.insert(t).second) return Problem::BadMap;
    }
    std::map<std::string, std::vector<const Hit*>> groups;
    for (const auto& h : sc.hits)
        if (eff(h.slot) != h.slot) groups[h.parent].push_back(&h);
    if (groups.empty()) return Problem::NoSlots;  // nada que cambiar

    std::vector<std::string> order;
    for (const auto& g : groups) order.push_back(g.first);
    std::sort(order.begin(), order.end(), [](const std::string& x, const std::string& y) {
        const int dx = depthOf(x), dy = depthOf(y);
        return dx != dy ? dx > dy : x < y;
    });

    int tmpCounter = 0;
    for (const auto& parent : order) {
        const auto& hs = groups[parent];
        std::set<std::string> movingOld, newNames;
        for (const Hit* h : hs) movingOld.insert(util::lower(h->name));
        std::vector<std::string> newName, tmpName;
        for (const Hit* h : hs) {
            std::string nn = h->name;
            nn.replace(h->pos, 2, twoDigits(eff(h->slot)));
            const std::string ln = util::lower(nn);
            if (!newNames.insert(ln).second) return Problem::BadMap;                            // dos elementos acabarían con el mismo nombre
            if (util::exists(util::join(parent, nn)) && !movingOld.count(ln)) return Problem::BadMap;  // el destino ya existe y no se mueve
            newName.push_back(nn);
            std::string tn;
            do {
                tn = "__reslot" + std::to_string(tmpCounter++);
            } while (util::exists(util::join(parent, tn)));
            tmpName.push_back(tn);
        }
        for (size_t i = 0; i < hs.size(); i++) plan.ops.push_back({util::join(parent, hs[i]->name), util::join(parent, tmpName[i])});
        for (size_t i = 0; i < hs.size(); i++) plan.ops.push_back({util::join(parent, tmpName[i]), util::join(parent, newName[i])});
        plan.items += (int)hs.size();
    }
    return Problem::None;
}

static bool doRename(const std::string& from, const std::string& to) { return ::rename(from.c_str(), to.c_str()) == 0; }

static void writeJournal(const Plan& plan, const std::string& path, bool& ok) {
    json::Value root = json::Value::Obj();
    json::Value ops = json::Value::Arr();
    for (const auto& op : plan.ops) {
        json::Value pair = json::Value::Arr();
        pair.push(json::Value::S(op.from));
        pair.push(json::Value::S(op.to));
        ops.push(pair);
    }
    root.set("ops", ops);
    ok = util::writeFileAtomic(path, root.dump());
}

Problem apply(const Plan& plan, const std::string& journalPath, std::atomic<int>* progress, const std::atomic<bool>* cancel, std::string& err) {
    err.clear();
    if (plan.ops.empty()) return Problem::None;
    bool ok = false;
    writeJournal(plan, journalPath, ok);
    if (!ok) {
        err = "no se pudo escribir el diario";
        return Problem::Io;
    }
    size_t done = 0;
    auto rollback = [&]() {
        bool all = true;
        while (done > 0) {
            const Op& op = plan.ops[done - 1];
            if (doRename(op.to, op.from)) done--;
            else {
                all = false;
                break;
            }
        }
        if (all) util::rmrf(journalPath);
        else err += " (no se pudo deshacer del todo: se terminará al reabrir la app)";
    };
    for (size_t i = 0; i < plan.ops.size(); i++) {
        if (cancel && cancel->load()) {
            rollback();
            return Problem::Cancelled;
        }
        if (testCrashAt >= 0 && (int)i == testCrashAt) {
            err = "apagón simulado";
            return Problem::Io;  // sin deshacer: el diario queda en disco
        }
        bool good;
        if (testFailAt >= 0 && (int)i == testFailAt) {
            good = false;
            errno = EIO;
        } else {
            good = doRename(plan.ops[i].from, plan.ops[i].to);
        }
        if (!good) {
            err = std::string(strerror(errno)) + ": " + plan.ops[i].from;
            rollback();
            return Problem::Io;
        }
        done = i + 1;
        if (progress) *progress = (int)done;
    }
    util::rmrf(journalPath);
    return Problem::None;
}

int recover(const std::string& journalPath, std::string& err) {
    err.clear();
    if (!util::isFile(journalPath)) return 0;
    std::string text;
    json::Value v;
    if (!util::readFile(journalPath, text) || !json::parse(text, v) || !v["ops"].isArray()) {
        err = "diario ilegible";
        util::moveDir(journalPath, journalPath + ".failed");
        return -1;
    }
    int resumed = 0;
    std::map<std::string, std::string> next;  // origen -> destino, para seguir la cadena nombre -> temporal -> final
    for (const auto& pair : v["ops"].elems()) next[pair.at(0).str()] = pair.at(1).str();
    for (const auto& pair : v["ops"].elems()) {
        const std::string from = pair.at(0).str(), to = pair.at(1).str();
        if (from.empty() || to.empty()) continue;
        const bool f = util::exists(from), t = util::exists(to);
        if (f && !t) {
            if (!doRename(from, to)) {
                err = std::string(strerror(errno)) + ": " + from;
                return -1;  // se conserva el diario: se reintentará la próxima vez
            }
            resumed++;
        } else if (!f && t) {
            continue;  // ya estaba hecho
        } else if (!f && !t && next.count(to) && util::exists(next[to])) {
            continue;  // hecho, y además ya pasó del nombre temporal al definitivo
        } else {
            err = "estado inesperado en " + from;
            util::moveDir(journalPath, journalPath + ".failed");
            return -1;
        }
    }
    util::rmrf(journalPath);
    return resumed;
}

}  // namespace reslot
