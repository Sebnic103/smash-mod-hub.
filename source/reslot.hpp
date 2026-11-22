#pragma once
// Reslotter: cambia el "slot" (c00..c07) de un mod de personaje renombrando las carpetas y archivos que lo llevan
// en el nombre (ARCropolis carga los archivos por ruta, así que basta con renombrar). Núcleo sin SDL/libnx:
// se prueba en el PC (tests/test_core.cpp).
//
// Seguridad: el renombrado se hace en dos fases por carpeta (origen -> nombre temporal -> destino, así se pueden
// intercambiar slots sin choques) y se apunta antes en un diario (journal). Si algo falla o se cancela, se
// deshace todo en orden inverso; si la consola se apaga a mitad, recover() termina el trabajo al abrir la app.
#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace reslot {

constexpr int kMaxSlot = 7;  // sin plugins ni config.json solo existen c00..c07

enum class Problem {
    None,
    NotFound,    // la carpeta del mod no existe
    HasConfig,   // trae config.json con rutas de slot (slots extra / archivos compartidos): no se toca
    ExtraSlots,  // usa slots c08 o superiores
    NoSlots,     // no se encontró ningún slot que cambiar
    BadMap,      // la correspondencia de slots no es válida (repetidos, fuera de rango o choca con otro slot)
    Io,          // error del sistema de archivos
    Cancelled,
};

struct Analysis {
    Problem problem = Problem::None;
    std::map<std::string, std::vector<int>> perFighter;  // personaje -> slots que usa el mod
    std::vector<int> slots;                              // todos los slots presentes (ordenados, sin repetir)
    int items = 0;                                       // carpetas/archivos que llevan slot en el nombre
    std::vector<std::string> unknown;                    // hasta 5 rutas con algo parecido a un slot que NO se reconoció
    bool ok() const { return problem == Problem::None; }
};

using SlotMap = std::map<int, int>;  // slot actual -> slot nuevo (los que no aparecen se quedan igual)

struct Op {
    std::string from, to;  // rutas completas
};

struct Plan {
    std::vector<Op> ops;  // en orden de ejecución (incluye los nombres temporales)
    int items = 0;        // elementos que cambian de nombre
};

// Lee el mod (solo nombres de carpetas/archivos, no el contenido) y detecta personajes y slots.
Analysis analyze(const std::string& modDir, const std::atomic<bool>* cancel = nullptr);
// personaje -> slots que usa el mod, aunque el mod no se pueda reslotear (para avisar de choques con otros mods).
std::map<std::string, std::vector<int>> fighterSlots(const std::string& modDir);

// Prepara los renombrados. Problem::None si todo está bien.
Problem makePlan(const std::string& modDir, const SlotMap& map, Plan& plan);
// Ejecuta el plan con diario. progress (opcional) cuenta operaciones hechas. Si cancel se activa, deshace.
Problem apply(const Plan& plan, const std::string& journalPath, std::atomic<int>* progress, const std::atomic<bool>* cancel, std::string& err);
// Termina (o ya terminó) un reslot interrumpido. Devuelve cuántas operaciones tuvo que completar (-1 = hubo un
// problema y el diario se conserva; 0 = no había nada pendiente).
int recover(const std::string& journalPath, std::string& err);

// true si config.json menciona slots (c00, _c01., chara_0_x_02.bntx...).
bool configMentionsSlots(const std::string& text);
// Nombre con el slot cambiado ("ef_mario_c01.eff", 3 -> "ef_mario_c03.eff"); "" si no lleva slot reconocible.
std::string renamedWithSlot(const std::string& name, int newSlot);

// Solo para los tests: provocar un fallo (o un "apagón") tras N renombrados. -1 = desactivado.
extern int testFailAt;
extern int testCrashAt;

}  // namespace reslot
