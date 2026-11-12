#include "http.hpp"

#include <curl/curl.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

#include "util.hpp"

namespace http {

static std::string g_ca;

std::string Result::describe() const {
    if (!error.empty()) return error;
    return "HTTP " + std::to_string(status);
}

void init(const std::string& caPath) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_ca = util::isFile(caPath) ? caPath : std::string();
    if (g_ca.empty()) util::log("http: no hay cacert.pem, TLS sin verificar");
}

void shutdown() { curl_global_cleanup(); }

bool tlsVerified() { return !g_ca.empty(); }

namespace {

struct WriteCtx {
    std::string* str = nullptr;
    FILE* file = nullptr;
    Progress* prog = nullptr;
};

size_t writeCb(char* ptr, size_t sz, size_t n, void* ud) {
    auto* c = static_cast<WriteCtx*>(ud);
    size_t len = sz * n;
    if (c->prog && c->prog->cancel.load()) return 0;
    if (c->str) {
        if (c->str->size() + len > (16u << 20)) return 0;  // 16 MB máx. en memoria
        c->str->append(ptr, len);
        return len;
    }
    if (c->file) return fwrite(ptr, 1, len, c->file);
    return 0;
}

int xferCb(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* p = static_cast<Progress*>(ud);
    if (!p) return 0;
    p->now = (uint64_t)dlnow;
    p->total = (uint64_t)dltotal;
    return p->cancel.load() ? 1 : 0;
}

void setup(CURL* c, const std::string& url, WriteCtx* ctx, Progress* p, long timeoutSec) {
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "SmashModHub/1.0 (Nintendo Switch; homebrew)");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (timeoutSec > 0) {
        curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    } else {
        // Descargas grandes: sin límite total, pero se corta si la velocidad cae a ~0.
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 45L);
    }
    if (!g_ca.empty()) {
        curl_easy_setopt(c, CURLOPT_CAINFO, g_ca.c_str());
    } else {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferCb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, p);
}

Result finish(CURL* c, CURLcode rc) {
    Result r;
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    r.status = code;
    if (rc != CURLE_OK) r.error = std::string("curl: ") + curl_easy_strerror(rc);
    return r;
}

}  // namespace

Result get(const std::string& url, std::string& body, Progress* progress, long timeoutSec) {
    Result r;
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "curl_easy_init falló";
        return r;
    }
    body.clear();
    WriteCtx ctx;
    ctx.str = &body;
    ctx.prog = progress;
    setup(c, url, &ctx, progress, timeoutSec);
    CURLcode rc = curl_easy_perform(c);
    r = finish(c, rc);
    curl_easy_cleanup(c);
    util::log("GET %s -> %s (%zu bytes)", url.c_str(), r.describe().c_str(), body.size());
    return r;
}

Result download(const std::string& url, const std::string& dest, Progress* progress) {
    Result r;
    util::mkdirs(util::dirName(dest));
    std::string part = dest + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) {
        r.error = "no se pudo crear " + part;
        return r;
    }
    std::vector<char> fbuf(1 << 16);  // propio de cada llamada: varios hilos descargan a la vez
    setvbuf(f, fbuf.data(), _IOFBF, fbuf.size());
    CURL* c = curl_easy_init();
    if (!c) {
        fclose(f);
        unlink(part.c_str());
        r.error = "curl_easy_init falló";
        return r;
    }
    WriteCtx ctx;
    ctx.file = f;
    ctx.prog = progress;
    setup(c, url, &ctx, progress, 0);
    CURLcode rc = curl_easy_perform(c);
    r = finish(c, rc);
    curl_easy_cleanup(c);
    bool closed = fclose(f) == 0;
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
    util::log("DL %s -> %s", url.c_str(), r.describe().c_str());
    return r;
}

}  // namespace http
