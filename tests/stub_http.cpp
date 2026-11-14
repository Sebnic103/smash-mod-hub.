// Sustituto de http.cpp para los tests del host: devuelve respuestas enlatadas.
#include <map>
#include <string>
#include <vector>

#include "../source/http.hpp"

std::map<std::string, std::string> g_canned;       // url (prefijo) -> cuerpo
std::vector<std::string> g_requested;              // urls pedidas

namespace http {
std::string Result::describe() const { return error.empty() ? "HTTP " + std::to_string(status) : error; }
void init(const std::string&) {}
void shutdown() {}
bool tlsVerified() { return true; }
void trimIdle() {}
void abortAll() {}

Result get(const std::string& url, std::string& body, Progress*, long) {
    g_requested.push_back(url);
    Result r;
    for (const auto& kv : g_canned) {
        if (url.compare(0, kv.first.size(), kv.first) == 0) {
            body = kv.second;
            r.status = 200;
            return r;
        }
    }
    r.status = 404;
    return r;
}

Result download(const std::string&, const std::string&, Progress*) {
    Result r;
    r.error = "stub";
    return r;
}

Result downloadBig(const std::string&, const std::string&, Progress*, int) {
    Result r;
    r.error = "stub";
    return r;
}
}  // namespace http
