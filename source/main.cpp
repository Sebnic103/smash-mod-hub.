// Smash Mod Hub - gestor y descargador de mods de Super Smash Bros. Ultimate (Atmosphere / ARCropolis)
#ifdef __SWITCH__

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>

#include <cstdio>
#include <exception>

#include "app.hpp"
#include "platform.hpp"
#include "sys.hpp"
#include "ui.hpp"
#include "util.hpp"

// Red con más margen que el de por defecto: búferes TCP de recepción grandes (ventana TCP mayor = más velocidad
// en conexiones con latencia) y más sesiones BSD (con 3 solo pueden esperar datos 3 hilos a la vez; con las
// descargas, las imágenes y la API a la vez, el resto se quedaba en cola). Si el sistema rechaza la
// configuración se baja un escalón, y al final se usa la de por defecto.
static void initNetwork() {
    SocketInitConfig cfg = {};
    cfg.tcp_tx_buf_size = 0x8000;
    cfg.tcp_tx_buf_max_size = 0x20000;
    cfg.tcp_rx_buf_size = 0x20000;
    cfg.tcp_rx_buf_max_size = 0x100000;
    cfg.udp_tx_buf_size = 0x2400;
    cfg.udp_rx_buf_size = 0xA500;
    cfg.sb_efficiency = 8;
    cfg.bsd_service_type = BsdServiceType_User;
    const u32 tries[] = {8, 4};
    for (u32 sessions : tries) {
        cfg.num_bsd_sessions = sessions;
        if (R_SUCCEEDED(socketInitialize(&cfg))) return;
    }
    socketInitializeDefault();
}

int main(int, char**) {
    sys::initThreads();
    romfsInit();
    initNetwork();
    plInitialize(PlServiceType_User);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
    TTF_Init();

    SDL_Window* win = SDL_CreateWindow("Smash Mod Hub", 0, 0, 1280, 720, SDL_WINDOW_FULLSCREEN);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;

    PlFontData font;
    bool fontOk = R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard));

    int rc = 0;
    if (ren && fontOk && ui::init(ren, font.address, font.size)) {
        platform::Pad pad;
        pad.init();
        App app;
        app.init(ren);
        int faults = 0;  // fotogramas seguidos que terminaron en excepción
        while (appletMainLoop() && !app.wantsQuit()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
            }
            // Una excepción en un fotograma (sin memoria, dato raro...) se anota y se sigue en vez de cerrar la app
            // de golpe. Si se repite sin parar algo va muy mal: se sale limpiamente (guardando todo).
            try {
                app.frame(pad.poll());
                faults = 0;
            } catch (const std::exception& ex) {
                util::log("EXCEPCIÓN en el fotograma: %s", ex.what());
                faults++;
            } catch (...) {
                util::log("EXCEPCIÓN desconocida en el fotograma");
                faults++;
            }
            if (faults > 0) {
                ui::unclip();  // por si la excepción dejó un recorte activo
                if (faults >= 30) {
                    util::log("demasiadas excepciones seguidas: se cierra la app");
                    break;
                }
            }
            SDL_RenderPresent(ren);
        }
        app.shutdown();
        ui::shutdown();
    } else {
        rc = 2;
    }

    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    plExit();
    socketExit();
    romfsExit();
    return rc;
}

#else
#error "Este programa solo se compila para Nintendo Switch (devkitPro / libnx). Para probar el núcleo en tu PC usa tests/run.sh"
#endif
