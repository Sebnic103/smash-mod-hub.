#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Metadatos que guardamos nosotros (mods.json); no tocamos las carpetas de los mods.
struct Meta {
    int64_t gbId = 0;
    std::string model;  // "Mod" / "Sound"
    std::string name, author, version, category, thumb, installedAt;
    int64_t fileId = 0;        // archivo de GameBanana instalado
    int64_t latestFileId = 0;  // el más reciente conocido (para avisar de actualizaciones)
};

struct Mod {
    std::string folder;  // nombre de carpeta (clave)
    bool enabled = true;
    Meta meta;
    // Datos de info.toml (opcional, lo usa ARCropolis)
    std::string infoName, infoAuthor, infoVersion, infoCategory;
    std::string localPreview;  // preview.webp/png/jpg dentro del mod, si existe
    uint64_t size = 0;
    bool sizeKnown = false;

    bool hasUpdate() const { return meta.gbId && meta.latestFileId > meta.fileId && meta.fileId > 0; }
    std::string title() const { return !meta.name.empty() ? meta.name : (!infoName.empty() ? infoName : folder); }
    std::string author() const { return !meta.author.empty() ? meta.author : infoAuthor; }
    std::string category() const { return !meta.category.empty() ? meta.category : infoCategory; }
    std::string version() const { return !meta.version.empty() ? meta.version : infoVersion; }
    std::string dir() const;  // ruta completa actual
    // Imagen a mostrar: miniatura descargada o preview.webp del propio mod ("" si no hay)
    std::string imagePath() const;
};

struct Conflict {
    std::string path;                // ruta relativa dentro del mod (en minúsculas)
    std::vector<std::string> mods;   // carpetas de los mods que la contienen
};

class ModStore {
public:
    // Escanea ultimate/mods y ultimate/mods_disabled y fusiona con mods.json
    void load();
    void save() const;

    std::vector<Mod>& mods() { return mods_; }
    const std::vector<Mod>& mods() const { return mods_; }
    int findByFolder(const std::string& folder) const;
    int findByGb(int64_t gbId) const;
    int enabledCount() const;

    bool setEnabled(size_t idx, bool enabled, std::string& err);
    bool remove(size_t idx, std::string& err);
    void recordInstall(const std::string& folder, const Meta& meta);  // no recarga; llamar a load() después
    void setLatest(int64_t gbId, int64_t latestFileId);
    void setSize(const std::string& folder, uint64_t size);

    // Perfiles: conjuntos de mods activados
    std::vector<std::string> profileNames() const;
    const std::vector<std::string>* profile(const std::string& name) const;
    void saveProfile(const std::string& name);
    bool deleteProfile(const std::string& name);
    // Aplica un perfil moviendo carpetas. Devuelve cuántos mods cambiaron de estado.
    int applyProfile(const std::string& name, std::string& err);

    int setAll(bool enabled, std::string& err);

private:
    void loadDb();
    std::vector<Mod> mods_;
    std::map<std::string, Meta> db_;
    std::map<std::string, std::vector<std::string>> profiles_;
    std::map<std::string, uint64_t> sizeCache_;
};

// Busca archivos repetidos entre mods activados. Pensado para correr en un hilo aparte.
// folders: carpetas (nombres) de los mods ACTIVADOS dentro de paths::mods().
std::vector<Conflict> scanConflicts(const std::vector<std::string>& folders, std::atomic<int>* done, std::atomic<bool>* cancel);
