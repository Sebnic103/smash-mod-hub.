#pragma once
// Música de fondo: conecta el motor (music.cpp) con el audio de SDL. Todo está protegido: si no hay dispositivo
// de audio, la app sigue funcionando sin música; una pista rara o un error se anota y se pasa a la siguiente.
#include <string>

namespace audio {

enum { kOff = 0, kChip = 1, kCustom = 2 };

bool init(int mode, int volumePct);  // false = sin audio (la app sigue igual)
void shutdown();
bool available();

// Devuelve el modo realmente aplicado (si no hay archivos .wav, "personalizada" se queda en el modo anterior).
int setMode(int mode);
int mode();
void setVolume(int pct);
void next();   // siguiente canción / archivo
void pump();   // llamar una vez por frame desde el hilo principal (carga y cambia de pista)

float level();  // 0..1 para el ecualizador de la interfaz
std::string nowPlaying();
int customCount();  // archivos .wav encontrados
void rescan();      // vuelve a leer la carpeta de música
std::string customDir();
std::string lastError();  // último problema con un archivo personalizado ("" si no hay)

}  // namespace audio
