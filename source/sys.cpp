#include "sys.hpp"

#include <atomic>
#include <chrono>

namespace sys {

#ifdef __SWITCH__

namespace {
uint64_t g_coreMask = 0;  // núcleos que el proceso puede usar (bit n = núcleo n)
int g_mainCore = -1;      // núcleo del hilo principal
std::atomic<unsigned> g_nextCore{0};

// Núcleo para el siguiente hilo de fondo: uno distinto al principal, repartiendo; -2 = el de por defecto.
int pickCore() {
    if (!g_coreMask) return -2;
    int cand[4];
    int n = 0;
    for (int c = 0; c < 4; c++)
        if (((g_coreMask >> c) & 1) && c != g_mainCore) cand[n++] = c;
    int m = 0;  // el núcleo 3 lo comparte el sistema: solo se usa si no hay otro
    for (int i = 0; i < n; i++)
        if (cand[i] < 3) cand[m++] = cand[i];
    if (m > 0) n = m;
    if (n == 0) return -2;
    return cand[g_nextCore.fetch_add(1) % (unsigned)n];
}
}  // namespace

void initThreads() {
    uint64_t mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) g_coreMask = mask;
    g_mainCore = (int)svcGetCurrentProcessorNumber();
}

Mutex::Mutex() { mutexInit(&m_); }
void Mutex::lock() { mutexLock(&m_); }
void Mutex::unlock() { mutexUnlock(&m_); }

void Thread::entry(void* arg) { static_cast<Thread*>(arg)->fn_(); }

bool Thread::start(std::function<void()> fn, size_t stackSize) {
    join();
    fn_ = std::move(fn);
    // Prioridad 0x3B (59) = menor que la del hilo principal (0x2C) y con reparto de tiempo entre hilos iguales.
    Result rc = threadCreate(&t_, &Thread::entry, this, nullptr, stackSize, 0x3B, pickCore());
    if (R_FAILED(rc)) rc = threadCreate(&t_, &Thread::entry, this, nullptr, stackSize, 0x3B, -2);
    if (R_FAILED(rc)) rc = threadCreate(&t_, &Thread::entry, this, nullptr, stackSize, 0x2C, -2);  // plan C: como antes
    if (R_FAILED(rc)) return false;
    if (R_FAILED(threadStart(&t_))) {
        threadClose(&t_);
        return false;
    }
    started_ = true;
    return true;
}

void Thread::join() {
    if (!started_) return;
    threadWaitForExit(&t_);
    threadClose(&t_);
    started_ = false;
}

void sleepMs(int ms) { svcSleepThread((int64_t)ms * 1000000LL); }

#else

void initThreads() {}

Mutex::Mutex() {}
void Mutex::lock() { m_.lock(); }
void Mutex::unlock() { m_.unlock(); }

bool Thread::start(std::function<void()> fn, size_t) {
    join();
    fn_ = std::move(fn);
    t_ = std::thread([this] { fn_(); });
    started_ = true;
    return true;
}

void Thread::join() {
    if (!started_) return;
    if (t_.joinable()) t_.join();
    started_ = false;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

#endif

uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace sys
