#include "http.hpp"

#include <curl/curl.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "segdl.hpp"
#include "sys.hpp"
#include "util.hpp"

namespace http {

static std::string g_ca;
static std::atomic<bool> g_abort{false};  // true al salir: corta todas las transferencias en curso

std::string Result::describe() const {
    if (!error.empty()) return error;
    return "HTTP " + std::to_string(status);
}

// ---------------------------------------------------------------------------------------------
// Reserva de "easy handles". Reutilizar el handle mantiene viva la conexión TLS (keep-alive), la caché de
// DNS y las sesiones TLS: abrir una conexión nueva en la Switch cuesta cientos de ms de CPU (apretón de
// manos TLS + leer cacert.pem), y antes se hacía una por cada imagen.
// ---------------------------------------------------------------------------------------------
namespace {

struct Pooled {
    CURL* h;
    std::string host;
    uint64_t since;  // cuándo quedó libre (ms)
};
constexpr uint64_t kIdleMs = 30000;  // una conexión sin usar más de 30 s se cierra
sys::Mutex g_poolMx;
std::vector<Pooled> g_pool;
constexpr size_t kPoolMax = 6;
void dropStaleLocked(uint64_t now);

std::string hostOf(const std::string& url) {
    size_t s = url.find("://");
    s = s == std::string::npos ? 0 : s + 3;
    size_t e = url.find_first_of("/?#", s);
    return url.substr(s, e == std::string::npos ? std::string::npos : e - s);
}

CURL* acquire(const std::string& host) {
    {
        sys::Lock l(g_poolMx);
        dropStaleLocked(sys::nowMs());
        for (size_t i = g_pool.size(); i-- > 0;) {
            if (g_pool[i].host == host) {
                CURL* h = g_pool[i].h;
                g_pool.erase(g_pool.begin() + (long)i);
                return h;
            }
        }
        if (!g_pool.empty()) {
            CURL* h = g_pool.back().h;
            g_pool.pop_back();
            return h;
        }
    }
    return curl_easy_init();
}

// keep=false cuando la transferencia falló: la conexión puede estar rota, mejor tirar el handle.
void release(CURL* h, const std::string& host, bool keep) {
    if (!h) return;
    if (!keep) {
        curl_easy_cleanup(h);
        return;
    }
    sys::Lock l(g_poolMx);
    const uint64_t now = sys::nowMs();
    dropStaleLocked(now);
    if (g_pool.size() >= kPoolMax) {
        curl_easy_cleanup(g_pool.front().h);
        g_pool.erase(g_pool.begin());
    }
    g_pool.push_back({h, host, now});
}

// Cierra los handles que llevan mucho sin usarse (con sus sockets). Hay que tener g_poolMx.
void dropStaleLocked(uint64_t now) {
    for (size_t i = 0; i < g_pool.size();) {
        if (now - g_pool[i].since > kIdleMs) {
            curl_easy_cleanup(g_pool[i].h);
            g_pool.erase(g_pool.begin() + (long)i);
        } else {
            i++;
        }
    }
}

void drainPool() {
    sys::Lock l(g_poolMx);
    for (auto& p : g_pool) curl_easy_cleanup(p.h);
    g_pool.clear();
}

// ---------------------------------------------------------------------------------------------
struct WriteCtx {
    std::string* str = nullptr;
    FILE* file = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    std::atomic<uint64_t>* received = nullptr;  // suma de bytes recibidos (para el progreso)
    size_t cap = 0;                             // máximo de bytes en memoria
};

size_t writeCb(char* ptr, size_t sz, size_t n, void* ud) {
    auto* c = static_cast<WriteCtx*>(ud);
    const size_t len = sz * n;
    if (g_abort.load() || (c->cancel && c->cancel->load())) return 0;
    size_t done = 0;
    if (c->str) {
        if (c->str->size() + len > c->cap) return 0;
        c->str->append(ptr, len);
        done = len;
    } else if (c->file) {
        done = fwrite(ptr, 1, len, c->file);
    }
    if (c->received && done) c->received->fetch_add(done);
    return done;
}

struct XferCtx {
    const std::atomic<bool>* cancel = nullptr;
    Progress* prog = nullptr;
    SpeedMeter meter;
};

int xferCb(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* x = static_cast<XferCtx*>(ud);
    if (x->prog) {
        x->prog->now = (uint64_t)dlnow;
        if (dltotal > 0) x->prog->total = (uint64_t)dltotal;
        x->prog->bps = x->meter.update(sys::nowMs(), (uint64_t)dlnow);
    }
    return (g_abort.load() || (x->cancel && x->cancel->load())) ? 1 : 0;
}

enum class Kind { Json, Image, Bulk };

void setupCommon(CURL* c, const std::string& url, Kind kind) {
    curl_easy_reset(c);  // conserva las conexiones vivas, la caché de DNS y las sesiones TLS
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "SmashModHub/1.0 (Nintendo Switch; homebrew)");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);  // sin HTTP/2: más simple y estable
    if (kind == Kind::Json) curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // gzip/deflate solo para JSON
    if (kind == Kind::Bulk) curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 131072L);  // menos vueltas por el bucle de lectura
    if (!g_ca.empty()) {
        curl_easy_setopt(c, CURLOPT_CAINFO, g_ca.c_str());
    } else {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}

void setupWrite(CURL* c, WriteCtx* w, XferCtx* x) {
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, w);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferCb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, x);
}

Result finish(CURL* c, CURLcode rc) {
    Result r;
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    r.status = code;
    if (rc != CURLE_OK) r.error = std::string("curl: ") + curl_easy_strerror(rc);
    return r;
}

// ---------------------------------------------------------------------------------------------
// Transporte curl para la descarga en trozos.
// ---------------------------------------------------------------------------------------------
struct ProbeCtx {
    size_t bytes = 0;
    std::string contentRange;
};

size_t probeWrite(char*, size_t sz, size_t n, void* ud) {
    auto* p = static_cast<ProbeCtx*>(ud);
    p->bytes += sz * n;
    return p->bytes > 64 ? 0 : sz * n;  // pedimos 1 byte: si llega más, el servidor ignora Range -> cortar
}

size_t headerCb(char* buf, size_t sz, size_t n, void* ud) {
    auto* p = static_cast<ProbeCtx*>(ud);
    const size_t len = sz * n;
    std::string line(buf, len);
    for (auto& ch : line) ch = (char)tolower((unsigned char)ch);
    if (line.compare(0, 5, "http/") == 0) p->contentRange.clear();  // nueva respuesta (redirecciones)
    else if (line.compare(0, 14, "content-range:") == 0) p->contentRange = util::trim(line.substr(14));
    return len;
}

class CurlTransport : public Transport {
public:
    const std::atomic<bool>* userCancel = nullptr;  // cancelación de la descarga (para la sonda)

    Probe probe(const std::string& url) override {
        Probe p;
        const std::string host = hostOf(url);
        CURL* c = acquire(host);
        if (!c) {
            p.error = "curl_easy_init falló";
            return p;
        }
        ProbeCtx ctx;
        XferCtx xc;
        xc.cancel = userCancel;
        setupCommon(c, url, Kind::Bulk);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferCb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &xc);
        curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 40L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probeWrite);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headerCb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &ctx);
        CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        char* eff = nullptr;
        curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
        if (eff && *eff) p.url = eff;
        curl_off_t clen = -1;
        curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &clen);
        p.status = code;
        bool healthy = false;
        if (code == 206) {
            // "bytes 0-0/12345"
            size_t slash = ctx.contentRange.find('/');
            if (slash != std::string::npos) {
                unsigned long long total = strtoull(ctx.contentRange.c_str() + slash + 1, nullptr, 10);
                if (total > 0) {
                    p.ok = true;
                    p.ranges = true;
                    p.size = total;
                    healthy = rc == CURLE_OK;
                }
            }
            if (!p.ok) p.error = "Content-Range ilegible";
        } else if (code == 200) {
            p.ok = true;  // el servidor ignora Range
            p.ranges = false;
            p.size = clen > 0 ? (uint64_t)clen : 0;
        } else {
            p.error = rc != CURLE_OK ? std::string("curl: ") + curl_easy_strerror(rc) : "HTTP " + std::to_string(code);
        }
        release(c, host, healthy);
        if (p.url == url) p.url.clear();
        return p;
    }

    FetchResult fetchRange(const std::string& url, uint64_t from, uint64_t to, std::string& out, const std::atomic<bool>& cancel, std::atomic<uint64_t>& received) override {
        FetchResult fr;
        const std::string host = hostOf(url);
        CURL* c = acquire(host);
        if (!c) {
            fr.st = Fetch::Retry;
            fr.error = "curl_easy_init falló";
            return fr;
        }
        const uint64_t len = to - from + 1;
        WriteCtx w;
        w.str = &out;
        w.cap = (size_t)len;
        w.cancel = &cancel;
        w.received = &received;
        XferCtx x;
        x.cancel = &cancel;
        setupCommon(c, url, Kind::Bulk);
        setupWrite(c, &w, &x);
        char range[64];
        snprintf(range, sizeof range, "%llu-%llu", (unsigned long long)from, (unsigned long long)to);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
        CURLcode rc = curl_easy_perform(c);
        Result r = finish(c, rc);
        const bool good = rc == CURLE_OK && r.status == 206 && out.size() == len;
        release(c, host, good);
        if (cancel.load()) {
            fr.st = Fetch::Cancelled;
            return fr;
        }
        if (good) return fr;
        if (r.status == 200) {
            if (rc == CURLE_OK && from == 0 && out.size() == len) return fr;  // trozo único = archivo entero
            fr.st = Fetch::Unsupported;  // ignoró Range y quiso mandar el archivo entero
            fr.error = "Range ignorado";
            return fr;
        }
        fr.st = Fetch::Retry;
        fr.error = rc != CURLE_OK ? r.describe() : "HTTP " + std::to_string(r.status);
        return fr;
    }
};

// ---------------------------------------------------------------------------------------------
Result downloadSingle(const std::string& url, const std::string& dest, Progress* progress, Kind kind, bool quiet) {
    Result r;
    util::mkdirs(util::dirName(dest));
    const std::string part = dest + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) {
        r.error = "no se pudo crear " + part;
        return r;
    }
    std::vector<char> fbuf(1 << 17);  // propio de cada llamada: varios hilos descargan a la vez
    setvbuf(f, fbuf.data(), _IOFBF, fbuf.size());
    const std::string host = hostOf(url);
    CURL* c = acquire(host);
    if (!c) {
        fclose(f);
        unlink(part.c_str());
        r.error = "curl_easy_init falló";
        return r;
    }
    WriteCtx w;
    w.file = f;
    w.cancel = progress ? &progress->cancel : nullptr;
    XferCtx x;
    x.cancel = w.cancel;
    x.prog = progress;
    setupCommon(c, url, kind);
    setupWrite(c, &w, &x);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, kind == Kind::Bulk ? 45L : 30L);
    if (kind == Kind::Image) curl_easy_setopt(c, CURLOPT_TIMEOUT, 90L);
    CURLcode rc = curl_easy_perform(c);
    r = finish(c, rc);
    release(c, host, rc == CURLE_OK && r.ok());
    const bool closed = fclose(f) == 0;
    if (r.ok() && closed) {
        unlink(dest.c_str());
        if (rename(part.c_str(), dest.c_str()) != 0) {
            unlink(part.c_str());
            r.error = "no se pudo renombrar el archivo descargado";
        }
    } else {
        unlink(part.c_str());
        if (r.ok() && !closed) r.error = "error de escritura (¿SD llena?)";
    }
    if (!quiet || !r.ok()) util::log("DL %s -> %s", url.c_str(), r.describe().c_str());
    return r;
}

}  // namespace

void init(const std::string& caPath) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_ca = util::isFile(caPath) ? caPath : std::string();
    if (g_ca.empty()) util::log("http: no hay cacert.pem, TLS sin verificar");
}

void abortAll() { g_abort = true; }

void trimIdle() {
    sys::Lock l(g_poolMx);
    dropStaleLocked(sys::nowMs());
}

void shutdown() {
    drainPool();
    curl_global_cleanup();
}

bool tlsVerified() { return !g_ca.empty(); }

Result get(const std::string& url, std::string& body, Progress* progress, long timeoutSec) {
    Result r;
    const std::string host = hostOf(url);
    CURL* c = acquire(host);
    if (!c) {
        r.error = "curl_easy_init falló";
        return r;
    }
    body.clear();
    WriteCtx w;
    w.str = &body;
    w.cap = 16u << 20;  // 16 MB máx. en memoria
    w.cancel = progress ? &progress->cancel : nullptr;
    XferCtx x;
    x.cancel = w.cancel;
    x.prog = progress;
    setupCommon(c, url, Kind::Json);
    setupWrite(c, &w, &x);
    if (timeoutSec > 0) curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    CURLcode rc = curl_easy_perform(c);
    r = finish(c, rc);
    release(c, host, rc == CURLE_OK);
    if (!r.ok()) util::log("GET %s -> %s (%zu bytes)", url.c_str(), r.describe().c_str(), body.size());
    return r;
}

Result download(const std::string& url, const std::string& dest, Progress* progress) {
    return downloadSingle(url, dest, progress, Kind::Image, true);
}

Result downloadBig(const std::string& url, const std::string& dest, Progress* progress, int conns) {
    Result r;
    if (conns > 1) {
        CurlTransport tr;
        tr.userCancel = progress ? &progress->cancel : nullptr;
        Probe pr = tr.probe(url);
        if (progress && progress->cancel.load()) {
            r.error = "cancelado";
            return r;
        }
        util::log("DL sonda %s: HTTP %ld, rangos=%s, tamaño=%llu", url.c_str(), pr.status, pr.ranges ? "si" : "no", (unsigned long long)pr.size);
        SegOptions opt;
        opt.conns = conns;
        if (pr.ok && pr.ranges && pr.size >= 2 * opt.chunk) {
            util::mkdirs(util::dirName(dest));
            const std::string part = dest + ".part";
            FILE* f = fopen(part.c_str(), "wb");
            if (!f) {
                r.error = "no se pudo crear " + part;
                return r;
            }
            std::vector<char> fbuf(1 << 17);
            setvbuf(f, fbuf.data(), _IOFBF, fbuf.size());
            SegResult sr = segmentedDownload(tr, url, pr, f, progress, opt);
            const bool closed = fclose(f) == 0;
            if (sr.st == SegStatus::Done && closed) {
                unlink(dest.c_str());
                if (rename(part.c_str(), dest.c_str()) != 0) {
                    unlink(part.c_str());
                    r.error = "no se pudo renombrar el archivo descargado";
                    return r;
                }
                r.status = 200;
                util::log("DL %s -> OK (%d conexiones)", url.c_str(), conns);
                return r;
            }
            unlink(part.c_str());
            if (sr.st == SegStatus::Cancelled) {
                r.error = "cancelado";
                return r;
            }
            if (sr.st == SegStatus::Done || sr.diskError) {  // no se pudo cerrar/escribir el archivo
                r.error = sr.error.empty() ? "error de escritura (¿SD llena?)" : sr.error;
                util::log("DL %s -> %s", url.c_str(), r.error.c_str());
                return r;
            }
            util::log("DL %s: la descarga por trozos falló (%s); se reintenta con una sola conexión", url.c_str(), sr.error.c_str());
            if (progress) {
                progress->now = 0;
                progress->bps = 0;
            }
        }
    }
    return downloadSingle(url, dest, progress, Kind::Bulk, false);
}

}  // namespace http
