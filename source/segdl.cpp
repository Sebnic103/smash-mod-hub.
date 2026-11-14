#include "segdl.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include "sys.hpp"

namespace http {

namespace {

struct Slot {
    std::string data;
    bool ready = false;
};

struct Shared {
    Transport* t = nullptr;
    std::string url, effUrl;
    uint64_t size = 0, chunk = 0, nChunks = 0;
    int window = 12, attempts = 4;

    sys::Mutex mx;
    std::vector<Slot> slots;  // protegido por mx
    uint64_t nextChunk = 0;   // protegido por mx
    uint64_t writeIdx = 0;    // protegido por mx (lo avanza el hilo que escribe)
    std::string failMsg;      // protegido por mx

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> unsupported{false};
    std::atomic<uint64_t> doneBytes{0};  // bytes de trozos ya completos
    std::vector<std::unique_ptr<std::atomic<uint64_t>>> inflight;  // bytes del trozo en curso de cada hilo
};

void fail(Shared& s, const std::string& msg) {
    {
        sys::Lock l(s.mx);
        if (s.failMsg.empty()) s.failMsg = msg;
    }
    s.failed = true;
    s.stop = true;
}

void worker(Shared& s, int w) {
    std::atomic<uint64_t>& rx = *s.inflight[(size_t)w];
    while (!s.stop.load()) {
        uint64_t idx = 0;
        bool got = false, finished = false;
        {
            sys::Lock l(s.mx);
            if (s.nextChunk >= s.nChunks) finished = true;
            else if (s.nextChunk < s.writeIdx + (uint64_t)s.window) {
                idx = s.nextChunk++;
                got = true;
            }
        }
        if (finished) return;
        if (!got) {  // ventana llena: espera a que el escritor avance
            sys::sleepMs(4);
            continue;
        }
        const uint64_t from = idx * s.chunk;
        const uint64_t to = std::min(s.size, from + s.chunk) - 1;
        const uint64_t len = to - from + 1;
        std::string data;
        std::string lastErr = "error de red";
        bool ok = false;
        for (int attempt = 0; attempt < s.attempts && !s.stop.load(); attempt++) {
            // Los dos primeros intentos van a la URL final; si fallan, a la original (por si la final caduca).
            const std::string& u = (attempt < 2 && !s.effUrl.empty()) ? s.effUrl : s.url;
            data.clear();
            data.reserve((size_t)len);
            rx = 0;
            FetchResult r = s.t->fetchRange(u, from, to, data, s.stop, rx);
            if (r.st == Fetch::Ok && data.size() == len) {
                ok = true;
                break;
            }
            rx = 0;
            if (r.st == Fetch::Cancelled) return;
            if (r.st == Fetch::Unsupported) {
                s.unsupported = true;
                s.stop = true;
                return;
            }
            lastErr = r.st == Fetch::Ok ? "trozo incompleto" : r.error;
            if (attempt + 1 < s.attempts) {
                for (int i = 0; i < 5 * (attempt + 1) && !s.stop.load(); i++) sys::sleepMs(40);  // espera creciente
            }
        }
        if (!ok) {
            if (!s.stop.load()) fail(s, lastErr);
            return;
        }
        rx = 0;
        s.doneBytes += len;
        sys::Lock l(s.mx);
        s.slots[(size_t)idx].data = std::move(data);
        s.slots[(size_t)idx].ready = true;
    }
}

}  // namespace

SegResult segmentedDownload(Transport& t, const std::string& url, const Probe& probe, FILE* out, Progress* prog, const SegOptions& opt) {
    SegResult res;
    if (!probe.ok || !probe.ranges || probe.size == 0 || !out) {
        res.st = SegStatus::Unsupported;
        return res;
    }
    Shared s;
    s.t = &t;
    s.url = url;
    s.effUrl = probe.url;
    s.size = probe.size;
    s.chunk = std::max<uint64_t>(opt.chunk, 1);
    s.nChunks = (s.size + s.chunk - 1) / s.chunk;
    s.window = std::max(opt.window, 2);
    s.attempts = std::max(opt.attempts, 1);
    s.slots.resize((size_t)s.nChunks);
    const int nThreads = (int)std::max<uint64_t>(1, std::min<uint64_t>((uint64_t)std::max(opt.conns, 1), s.nChunks));
    for (int i = 0; i < nThreads; i++) s.inflight.emplace_back(new std::atomic<uint64_t>(0));

    std::vector<std::unique_ptr<sys::Thread>> threads;
    for (int i = 0; i < nThreads; i++) {
        auto th = std::make_unique<sys::Thread>();
        if (th->start([&s, i]() { worker(s, i); }, 0x100000)) threads.push_back(std::move(th));
        else break;
    }
    if (threads.empty()) {
        res.st = SegStatus::Unsupported;
        res.error = "no se pudieron crear hilos";
        return res;
    }

    std::atomic<bool> noCancel{false};
    const std::atomic<bool>& userCancel = prog ? prog->cancel : noCancel;
    if (prog) prog->total = s.size;
    SpeedMeter meter;
    uint64_t lastNow = 0;
    bool cancelled = false, writeErr = false;

    auto report = [&]() {
        if (!prog) return;
        uint64_t n = s.doneBytes.load();
        for (auto& f : s.inflight) n += f->load();
        n = std::min(n, s.size);
        if (n < lastNow) n = lastNow;  // nunca retrocede
        lastNow = n;
        prog->now = n;
        prog->bps = meter.update(sys::nowMs(), n);
    };

    while (true) {
        if (userCancel.load()) {
            cancelled = true;
            break;
        }
        std::string data;
        bool have = false, allDone = false;
        {
            sys::Lock l(s.mx);
            if (s.writeIdx >= s.nChunks) allDone = true;
            else if (s.slots[(size_t)s.writeIdx].ready) {
                data = std::move(s.slots[(size_t)s.writeIdx].data);
                s.slots[(size_t)s.writeIdx].data = std::string();
                have = true;
            }
        }
        if (allDone) break;
        if (have) {
            if (fwrite(data.data(), 1, data.size(), out) != data.size()) {
                writeErr = true;
                break;
            }
            sys::Lock l(s.mx);
            s.writeIdx++;
            report();
            continue;
        }
        if (s.stop.load()) break;  // fallo o "no soportado" de algún hilo
        report();
        sys::sleepMs(4);
    }

    s.stop = true;
    for (auto& th : threads) th->join();

    if (cancelled) {
        res.st = SegStatus::Cancelled;
    } else if (writeErr) {
        res.st = SegStatus::Failed;
        res.diskError = true;
        res.error = "error de escritura (¿SD llena?)";
    } else if (s.unsupported.load()) {
        res.st = SegStatus::Unsupported;
        res.error = "el servidor no admite trozos";
    } else if (s.failed.load()) {
        res.st = SegStatus::Failed;
        res.error = s.failMsg.empty() ? "error de red" : s.failMsg;
    } else {
        res.st = SegStatus::Done;
        if (prog) prog->now = s.size;
    }
    return res;
}

}  // namespace http
