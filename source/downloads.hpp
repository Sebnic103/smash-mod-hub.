#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "common.hpp"
#include "gamebanana.hpp"
#include "modstore.hpp"
#include "sys.hpp"

struct Job {
    enum State { Queued = 0, Downloading, Verifying, Extracting, Installing, Done, Failed, Cancelled };

    int id = 0;
    gb::Item item;
    gb::File file;
    std::string replaceFolder;  // si no está vacío: actualización de esa carpeta
    std::string targetDir;      // dónde instalar (mods o mods_disabled)
    std::string localPath;      // si no está vacío: archivo ya en la SD (importación), no se descarga

    std::atomic<int> state{Queued};
    Progress prog;  // descarga / verificación / extracción
    std::string error;                 // válido cuando state == Failed
    std::vector<std::string> folders;  // válido cuando state == Done
    Meta meta;                         // metadatos a registrar (válido cuando state == Done)
    bool reported = false;             // lo usa la interfaz
};

class Downloads {
public:
    ~Downloads() { stop(); }
    void start();
    void stop();

    std::shared_ptr<Job> enqueue(const gb::Item& item, const gb::File& file, const std::string& replaceFolder, const std::string& targetDir);
    // Instala un archivo que ya está en la SD (carpeta import). Al terminar lo mueve a import/done.
    std::shared_ptr<Job> enqueueLocal(const std::string& archivePath, const std::string& displayName, const std::string& targetDir);
    std::vector<std::shared_ptr<Job>> jobs();
    // Trabajos terminados (Done / Failed) que la interfaz aún no ha procesado.
    std::vector<std::shared_ptr<Job>> takeFinished();
    void cancel(int id);
    void remove(int id);  // solo si ya terminó
    void clearFinished();
    int activeCount();

private:
    void run();
    void process(Job& j);

    sys::Mutex mx_;
    std::vector<std::shared_ptr<Job>> jobs_;
    sys::Thread th_;
    std::atomic<bool> quit_{false};
    int nextId_ = 1;
};
