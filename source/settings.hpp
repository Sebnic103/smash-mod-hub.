#pragma once
#include <string>

struct Settings {
    bool english = false;       // idioma de la interfaz
    int gameId = 6498;          // ID de Super Smash Bros. Ultimate en GameBanana
    bool keepArchives = false;  // conservar los .zip/.7z/.rar descargados
    bool verifyMd5 = true;      // comprobar MD5 si GameBanana lo informa
    int perPage = 20;           // resultados por página
    int dlConns = 4;            // conexiones en paralelo para descargar un archivo (1 = una sola)
    bool systemKeyboard = false;  // usar el teclado del sistema (bloqueante) en vez del propio en pantalla
    int musicMode = 1;          // música: 0 = desactivada, 1 = chiptune propio, 2 = archivos .wav de la SD
    int volume = 60;            // volumen de la música, 0-100

    void load();
    void save() const;
};

extern Settings g_settings;

// Traducción mínima: T("texto en español", "english text")
inline const char* T(const char* es, const char* en) { return g_settings.english ? en : es; }
