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
        const bool started = th_.start([this, fn]() {
            T r{};
            // Una excepción (sin memoria, JSON raro...) no debe tumbar la app ni dejar la tarea "en curso" para siempre.
            try {
                r = fn();
            } catch (...) {
                sys::noteFault();
            }
            {
                sys::Lock l(mx_);
                res_ = std::move(r);
                ready_ = true;
            }
            busy = false;
        });
        if (!started) {  // no se pudo crear el hilo: se entrega un resultado vacío en vez de quedarse esperando
            sys::noteFault();
            sys::Lock l(mx_);
            res_ = T{};
            ready_ = true;
            busy = false;
        }
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
