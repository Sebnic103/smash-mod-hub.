#pragma once
#include <atomic>
#include <functional>
#include <utility>

#include "sys.hpp"

// Ejecuta una función en un hilo y entrega el resultado al hilo principal con take().
// Solo corre una a la vez: run() mientras busy es true se ignora.
template <class T>
class Async {
public:
    std::atomic<bool> busy{false};

    void run(std::function<T()> fn) {
        if (busy.load()) return;
        th_.join();
        {
            sys::Lock l(mx_);
            ready_ = false;
        }
        busy = true;
        th_.start([this, fn]() {
            T r = fn();
            {
                sys::Lock l(mx_);
                res_ = std::move(r);
                ready_ = true;
            }
            busy = false;
        });
    }

    bool take(T& out) {
        sys::Lock l(mx_);
        if (!ready_) return false;
        out = std::move(res_);
        ready_ = false;
        return true;
    }

    void join() { th_.join(); }

private:
    sys::Thread th_;
    sys::Mutex mx_;
    T res_{};
    bool ready_ = false;
};
