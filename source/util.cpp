#include "util.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sys.hpp"

// ---------------------------------------------------------------- paths
namespace paths {
static std::string g_root = "sdmc:";

void setRoot(const std::string& r) {
    g_root = r;
    while (g_root.size() > 1 && g_root.back() == '/') g_root.pop_back();
}
std::string root() { return g_root; }
std::string mods() { return g_root + "/ultimate/mods"; }
std::string modsDisabled() { return g_root + "/ultimate/mods_disabled"; }
std::string data() { return g_root + "/switch/smash-mod-hub"; }
std::string tmp() { return data() + "/tmp"; }
std::string downloads() { return data() + "/downloads"; }
std::string thumbs() { return data() + "/thumbs"; }
std::string cache() { return data() + "/cache"; }
std::string importDir() { return data() + "/import"; }
std::string dbFile() { return data() + "/mods.json"; }
std::string settingsFile() { return data() + "/settings.json"; }
std::string logFile() { return data() + "/log.txt"; }
std::string smashTitleDir() { return g_root + "/atmosphere/contents/01006A800016E000"; }
}  // namespace paths

// ---------------------------------------------------------------- util
namespace util {

static sys::Mutex g_logMx;
static std::string g_logPath;

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool startsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string baseName(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t i = s.find_last_of('/');
    return i == std::string::npos ? s : s.substr(i + 1);
}

std::string dirName(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t i = s.find_last_of('/');
    return i == std::string::npos ? std::string() : s.substr(0, i);
}

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool isFile(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool mkdirs(const std::string& p) {
    if (p.empty()) return false;
    if (isDir(p)) return true;
    for (size_t i = 1; i < p.size(); i++) {
        if (p[i] != '/') continue;
        std::string part = p.substr(0, i);
        if (part.empty() || part.back() == ':') continue;  // "sdmc:"
        if (!isDir(part)) mkdir(part.c_str(), 0777);
    }
    if (!isDir(p)) mkdir(p.c_str(), 0777);
    return isDir(p);
}

static std::vector<std::string> listAll(const std::string& p, int want /*0 any,1 dirs,2 files*/) {
    std::vector<std::string> out;
    DIR* d = opendir(p.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string name = e->d_name;
        if (want != 0) {
            bool dir = isDir(join(p, name));
            if ((want == 1) != dir) continue;
        }
        out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

std::vector<std::string> listDirs(const std::string& p) { return listAll(p, 1); }
std::vector<std::string> listFiles(const std::string& p) { return listAll(p, 2); }

uint64_t fileSize(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

bool rmrf(const std::string& p) {
    if (!exists(p)) return true;
    if (!isDir(p)) return unlink(p.c_str()) == 0;
    for (const auto& n : listAll(p, 0)) rmrf(join(p, n));
    return rmdir(p.c_str()) == 0;
}

uint64_t dirSize(const std::string& p) {
    uint64_t total = 0;
    for (const auto& n : listAll(p, 0)) {
        std::string f = join(p, n);
        if (isDir(f))
            total += dirSize(f);
        else
            total += fileSize(f);
    }
    return total;
}

bool copyFile(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    std::vector<char> buf(1 << 16);
    size_t n;
    bool ok = true;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0) {
        if (fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

static bool copyTree(const std::string& from, const std::string& to) {
    if (!mkdirs(to)) return false;
    for (const auto& n : listAll(from, 0)) {
        std::string a = join(from, n), b = join(to, n);
        if (isDir(a)) {
            if (!copyTree(a, b)) return false;
        } else if (!copyFile(a, b)) {
            return false;
        }
    }
    return true;
}

bool moveDir(const std::string& from, const std::string& to) {
    mkdirs(dirName(to));
    if (rename(from.c_str(), to.c_str()) == 0) return true;
    // Fallback: copiar y borrar (p. ej. entre volúmenes distintos)
    if (!copyTree(from, to)) {
        rmrf(to);
        return false;
    }
    return rmrf(from);
}

bool readFile(const std::string& p, std::string& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    std::vector<char> buf(1 << 16);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) out.append(buf.data(), n);
    fclose(f);
    return true;
}

bool writeFile(const std::string& p, const std::string& data) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool writeFileAtomic(const std::string& p, const std::string& data) {
    mkdirs(dirName(p));
    std::string tmp = p + ".tmp";
    if (!writeFile(tmp, data)) return false;
    unlink(p.c_str());
    if (rename(tmp.c_str(), p.c_str()) == 0) return true;
    unlink(tmp.c_str());
    return false;
}

uint64_t freeSpace(const std::string& p) {
    struct statvfs sv;
    if (statvfs(p.c_str(), &sv) != 0) return 0;
    uint64_t fr = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
    return (uint64_t)sv.f_bavail * fr;
}

std::string clipUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (((unsigned char)s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

static std::string sanitizeImpl(const std::string& s, bool allowLeadingDot) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 32 || strchr("<>:\"/\\|?*", c))
            o += '_';
        else
            o += (char)c;
    }
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    size_t lead = 0;
    while (lead < o.size() && (o[lead] == ' ' || (!allowLeadingDot && o[lead] == '.'))) lead++;
    o = o.substr(lead);
    o = clipUtf8(o, 80);
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    if (o.empty()) o = "mod";
    return o;
}

std::string sanitizeName(const std::string& s) { return sanitizeImpl(s, false); }

std::string sanitizeComponent(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 32 || strchr("<>:\"\\|?*", c))
            o += '_';
        else
            o += (char)c;
    }
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    return clipUtf8(o, 200);
}

std::string humanSize(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30))
        snprintf(buf, sizeof buf, "%.2f GB", b / (double)(1ull << 30));
    else if (b >= (1ull << 20))
        snprintf(buf, sizeof buf, "%.1f MB", b / (double)(1ull << 20));
    else if (b >= (1ull << 10))
        snprintf(buf, sizeof buf, "%.0f KB", b / (double)(1ull << 10));
    else
        snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    return buf;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

static void appendUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) {
        o += (char)cp;
    } else if (cp < 0x800) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
}

std::string stripHtml(const std::string& h) {
    std::string o;
    o.reserve(h.size());
    size_t i = 0, n = h.size();
    while (i < n) {
        char c = h[i];
        if (c == '<') {
            size_t e = h.find('>', i);
            if (e == std::string::npos) break;
            std::string tag = lower(h.substr(i + 1, e - i - 1));
            bool closing = !tag.empty() && tag[0] == '/';
            std::string body = closing ? tag.substr(1) : tag;
            size_t sp = body.find_first_of(" \t\r\n/");
            std::string name = body.substr(0, sp == std::string::npos ? body.size() : sp);
            if (name == "br" || (closing && (name == "p" || name == "div" || name == "ul" || name == "ol" || name == "h1" || name == "h2" || name == "h3" || name == "h4" || name == "h5" || name == "h6" || name == "tr")))
                o += '\n';
            else if (!closing && name == "li")
                o += "\n- ";
            i = e + 1;
        } else if (c == '&') {
            size_t e = h.find(';', i);
            if (e != std::string::npos && e - i <= 10) {
                std::string ent = h.substr(i + 1, e - i - 1);
                if (ent == "amp") o += '&';
                else if (ent == "lt") o += '<';
                else if (ent == "gt") o += '>';
                else if (ent == "quot") o += '"';
                else if (ent == "apos" || ent == "#39") o += '\'';
                else if (ent == "nbsp") o += ' ';
                else if (!ent.empty() && ent[0] == '#') {
                    unsigned cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? (unsigned)strtoul(ent.c_str() + 2, nullptr, 16) : (unsigned)strtoul(ent.c_str() + 1, nullptr, 10);
                    if (cp) appendUtf8(o, cp);
                } else {
                    o += h.substr(i, e - i + 1);
                }
                i = e + 1;
            } else {
                o += c;
                i++;
            }
        } else if (c == '\r') {
            i++;
        } else {
            o += c;
            i++;
        }
    }
    // Colapsar saltos de línea excesivos y espacios finales.
    std::string r;
    int nl = 0;
    for (char c : o) {
        if (c == '\n') {
            if (++nl <= 2) r += c;
        } else {
            nl = 0;
            r += c;
        }
    }
    return trim(r);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

std::string fnvHex(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return buf;
}

std::string formatDate(int64_t ts) {
    if (ts <= 0) return "";
    int64_t z = ts / 86400 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    char buf[64];
    snprintf(buf, sizeof buf, "%04lld-%02lld-%02lld", (long long)y, (long long)m, (long long)d);
    return buf;
}

void setLogPath(const std::string& p) {
    sys::Lock l(g_logMx);
    g_logPath = p;
    struct stat st;
    if (stat(p.c_str(), &st) == 0 && st.st_size > 256 * 1024) remove(p.c_str());
}

// El registro se guarda en memoria y se vuelca a la SD en bloques (abrir/cerrar el archivo por cada línea
// cuesta varias llamadas al sistema de archivos y frenaba a los hilos de red).
static std::string g_logBuf;
static uint64_t g_logFlushedAt = 0;

static void flushLogLocked() {
    if (!g_logBuf.empty() && !g_logPath.empty()) {
        FILE* f = fopen(g_logPath.c_str(), "a");
        if (f) {
            fwrite(g_logBuf.data(), 1, g_logBuf.size(), f);
            fclose(f);
        }
    }
    g_logBuf.clear();
    g_logFlushedAt = sys::nowMs();
}

void flushLog() {
    sys::Lock l(g_logMx);
    flushLogLocked();
}

void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sys::Lock l(g_logMx);
    if (g_logPath.empty()) return;
    char head[40];
    snprintf(head, sizeof head, "[%llu] ", (unsigned long long)(sys::nowMs() / 1000));
    g_logBuf += head;
    g_logBuf += buf;
    g_logBuf += '\n';
    const uint64_t now = sys::nowMs();
    if (g_logBuf.size() > 4096 || now - g_logFlushedAt > 3000) flushLogLocked();
}

}  // namespace util
