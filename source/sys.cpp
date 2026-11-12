#include "sys.hpp"

#include <chrono>

namespace sys {

#ifdef __SWITCH__

Mutex::Mutex() { mutexInit(&m_); }
void Mutex::lock() { mutexLock(&m_); }
void Mutex::unlock() { mutexUnlock(&m_); }

void Thread::entry(void* arg) { static_cast<Thread*>(arg)->fn_(); }

bool Thread::start(std::function<void()> fn, size_t stackSize) {
    join();
    fn_ = std::move(fn);
    if (R_FAILED(threadCreate(&t_, &Thread::entry, this, nullptr, stackSize, 0x2C, -2))) return false;
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
