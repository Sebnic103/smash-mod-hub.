#pragma once
#include <string>

struct Settings {
    bool english = false;       // idioma de la interfaz
    int gameId = 6498;          // ID de Super Smash Bros. Ultimate en GameBanana
    bool keepArchives = false;  // conservar los .zip/.7z/.rar descargados
    bool verifyMd5 = true;      // comprobar MD5 si GameBanana lo informa
    int perPage = 20;           // resultados por página

    void load();
    void save() const;
};

extern Settings g_settings;

// Traducción mínima: T("texto en español", "english text")
inline const char* T(const char* es, const char* en) { return g_settings.english ? en : es; }
