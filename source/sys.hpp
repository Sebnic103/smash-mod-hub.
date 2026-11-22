#pragma once
// Pequeña capa de hilos/mutex: usa libnx en la Switch y std:: en el host (tests).
#include <cstddef>
#include <cstdint>
#include <functional>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <mutex>
#include <thread>
#endif

namespace sys {

class Mutex {
public:
    Mutex();
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    void lock();
    void unlock();

private:
#ifdef __SWITCH__
    ::Mutex m_;
#else
    std::mutex m_;
#endif
};

class Lock {
public:
    explicit Lock(Mutex& m) : m_(m) { m_.lock(); }
    ~Lock() { m_.unlock(); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    Mutex& m_;
};

// Llamar una vez al arrancar (desde el hilo principal). En la Switch averigua qué núcleos tiene el proceso
// para poder mandar los hilos de fondo a núcleos distintos del de la interfaz.
void initThreads();

// Hilo de fondo: en la Switch corre con prioridad MENOR que el hilo principal (la interfaz lo interrumpe
// siempre) y, si hay otro núcleo libre, en ese núcleo. Así las descargas, el JSON, TLS o la decodificación
// de imágenes no dejan la interfaz congelada.
// Cuenta las excepciones que se atraparon en hilos de fondo (para el registro de diagnóstico).
void noteFault();
int faultCount();

class Thread {
public:
    Thread() = default;
    ~Thread() { join(); }
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // Lanza fn en un hilo nuevo (primero espera al anterior si seguía vivo).
    bool start(std::function<void()> fn, size_t stackSize = 0x100000);
    void join();

private:
    std::function<void()> fn_;
    bool started_ = false;
#ifdef __SWITCH__
    ::Thread t_;
    static void entry(void* arg);
#else
    std::thread t_;
#endif
};

void sleepMs(int ms);
uint64_t nowMs();  // reloj monotónico en milisegundos

}  // namespace sys
