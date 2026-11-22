#pragma once
#include <SDL2/SDL.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "async.hpp"
#include "downloads.hpp"
#include "gamebanana.hpp"
#include "imagecache.hpp"
#include "input.hpp"
#include "modstore.hpp"
#include "reslot.hpp"
#include "ui.hpp"

class App {
public:
    bool init(SDL_Renderer* r);
    void shutdown();
    void frame(const Input& in);  // actualiza y dibuja un frame (el llamador hace SDL_RenderPresent)
    bool wantsQuit() const { return quit_; }

private:
    enum Tab { TAB_BROWSE = 0, TAB_INSTALLED, TAB_DOWNLOADS, TAB_PROFILES, TAB_TOOLS, TAB_COUNT };

    struct DetailRes {
        bool ok = false;
        std::string err;
        gb::Detail d;
    };
    struct UpdRes {
        std::vector<std::pair<int64_t, int64_t>> latest;  // (gbId, último fileId)
    };
    struct ConflictRes {
        std::vector<Conflict> list;
        bool cancelled = false;
    };
    struct SizeRes {
        std::vector<std::pair<std::string, uint64_t>> sizes;  // (carpeta, bytes)
    };
    // --- filtros por categoría
    struct CatRes {
        int64_t parent = 0;
        bool drill = false;  // se pidieron los hijos de "target" (el usuario pulsó A sobre ella)
        gb::Category target;
        gb::CategoryList list;
        int token = 0;  // a qué apertura del selector pertenece (las respuestas viejas se descartan)
    };
    struct CatLevel {
        int64_t id = 0;
        std::string name;
        std::vector<gb::Category> cats;
        bool learned = false, stale = false;
    };
    struct CatPicker {
        bool open = false, loading = false;
        std::vector<CatLevel> levels;  // levels[0] = raíz
        std::string filter, err;       // filtro de texto dentro de la lista / último error
        std::vector<int> shown;        // posiciones de levels.back().cats que pasan el filtro
        int sel = 0;                   // 0 = fila especial; 1.. = shown[sel - 1]
        int token = 0;
        bool pendingRoot = false;      // se abrió mientras aún corría una petición anterior
    };
    // --- reslotter
    struct ReslotOther {
        std::string title;
        std::map<std::string, std::vector<int>> use;  // personaje -> slots que usa ese otro mod
    };
    struct ReslotAnalysisRes {
        std::string folder;
        reslot::Analysis an;
        std::vector<ReslotOther> others;
    };
    struct ReslotApplyRes {
        reslot::Problem p = reslot::Problem::None;
        std::string err, folder;
        int items = 0;
    };
    struct ReslotUi {
        bool open = false, analyzing = false, applying = false;
        std::string folder, title, dir;
        reslot::Analysis an;
        std::vector<ReslotOther> others;
        std::vector<int> src, dst;
        int sel = 0;
    };
    struct Badge {
        std::string text;
        ui::Color col;
    };
    struct Card {
        SDL_Texture* tex = nullptr;
        std::string title, l2, l3;
        bool selected = false, dim = false, noImage = false;
        std::vector<Badge> badges;
    };
    struct Scroller {
        float target = 0, cur = 0;
        void step() {
            cur += (target - cur) * 0.28f;
            if (cur - target < 0.5f && target - cur < 0.5f) cur = target;
        }
        void reset() { target = cur = 0; }
    };
    struct Dialog {
        bool open = false, confirm = true;
        std::string title, msg;
        std::function<void()> onYes;
    };
    struct TextView {
        bool open = false;
        std::string title;
        std::vector<std::string> lines;
        float scroll = 0;
    };

    // --- ciclo de vida / utilidades
    void pollAsync();
    void handleFinishedJobs();
    void toast(const std::string& msg, ui::Color c);
    void confirm(const std::string& title, const std::string& msg, std::function<void()> onYes);
    void notice(const std::string& title, const std::string& msg);
    void openView(const std::string& title, std::vector<std::string> lines);
    void refreshStatus();

    // --- explorar
    void loadPage(int page);
    std::string pageKey(int page) const;
    void startPageJob();
    void applyPage(gb::Page&& pg);
    void rememberPage(const std::string& key, const gb::Page& pg);
    void refreshImportCount();
    void updateBrowse(const Input& in);
    void drawBrowse();
    void openDetail(const gb::Item& it);
    void updateDetail(const Input& in);
    void drawDetail();
    void queueDownload(const gb::Item& item, const gb::File& f);
    void openCatPicker();
    void requestCats(const gb::Category* target);
    void rebuildShown();
    void applyCategory(int64_t id, const std::string& name);
    std::string catPathName(const gb::Category* extra) const;
    void updateCatPicker(const Input& in);
    void drawCatPicker();
    void queueNow(const gb::Item& item, const gb::File& f, const std::string& replace, const std::string& target);

    // --- instalados
    void updateInstalled(const Input& in);
    void drawInstalled();
    void startUpdateCheck(bool verbose);
    void startModUpdate(size_t idx);
    void startSizeScan();
    static gb::Item itemFromMod(const Mod& m);

    // --- reslotter
    void openReslot(size_t idx);
    void updateReslot(const Input& in);
    void drawReslot();
    bool reslotValid() const;
    std::vector<std::string> reslotWarnings() const;
    void startReslot();

    // --- descargas / perfiles / herramientas
    void updateDownloads(const Input& in);
    void drawDownloads();
    void updateProfiles(const Input& in);
    void drawProfiles();
    void updateTools(const Input& in);
    void drawTools();
    void activateTool(int idx);
    std::string toolLabel(int idx, std::string& value) const;
    void startConflictScan();
    void runImport();
    void showDiagnostics();

    // --- piezas de interfaz
    void updateMain(const Input& in);
    void drawMain();
    void drawHeader();
    void drawFooter(const std::string& hints);
    void drawCard(int x, int y, int w, int h, const Card& c);
    void drawBar(int x, int y, int w, int h, double frac, ui::Color c);
    void drawCenterMessage(const std::string& title, const std::string& sub, ui::Color c);
    void drawToast();
    void drawDialog();
    void drawView();
    void drawBusyOverlay(const std::string& msg, double frac);
    void updateDialog(const Input& in);
    void updateView(const Input& in);
    bool moveSel(int& sel, int count, int cols, const Input& in);
    void ensureVisible(int sel, int cols, Scroller& s, int viewH);
    std::string tabName(int t);

    // --- estado
    SDL_Renderer* ren_ = nullptr;
    ImageCache img_;
    ModStore store_;
    Downloads dl_;
    uint64_t now_ = 0;
    bool quit_ = false;
    int tab_ = TAB_BROWSE;
    std::atomic<bool> bgCancel_{false};

    // explorar
    gb::Page page_;
    Async<gb::Page> pageJob_;
    bool pageLoaded_ = false;
    int pageNo_ = 1;
    gb::Sort sort_ = gb::Sort::Default;
    std::string query_, pageErr_;
    int sel_ = 0;
    Scroller browseScroll_;
    // Páginas ya vistas (ir atrás/adelante es instantáneo) y petición pendiente mientras otra carga.
    std::map<std::string, gb::Page> pageCache_;
    std::vector<std::string> pageOrder_;
    int64_t catId_ = 0;  // categoría activa (0 = todas)
    std::string catName_;
    CatPicker cp_;
    Async<CatRes> catJob_;
    int catToken_ = 0;
    std::string desiredKey_, jobKey_;
    int desiredPage_ = 1;
    bool wantPending_ = false;

    // detalle
    bool detailOpen_ = false, detailPending_ = false, detLoaded_ = false;
    gb::Detail det_;
    std::string detErr_;
    Async<DetailRes> detailJob_;
    int detFile_ = 0, detImg_ = 0;
    float detTextScroll_ = 0;

    // instalados
    int instSel_ = 0;
    Scroller instScroll_;
    Async<UpdRes> updJob_;
    std::atomic<int> updDone_{0};
    int updTotal_ = 0;
    bool updVerbose_ = false;
    Async<DetailRes> updPrep_;
    std::string updFolder_, updTarget_;
    Async<SizeRes> sizeJob_;
    bool sizesDirty_ = true;

    // reslotter
    ReslotUi rs_;
    Async<ReslotAnalysisRes> rsAnalyze_;
    Async<ReslotApplyRes> rsApply_;
    std::atomic<int> rsDone_{0}, rsTotal_{0};
    std::atomic<bool> rsCancel_{false};

    // descargas / perfiles / herramientas
    int dlSel_ = 0, profSel_ = 0, toolSel_ = 0;
    Async<ConflictRes> confJob_;
    std::atomic<int> confDone_{0};
    std::atomic<bool> confCancel_{false};
    int confTotal_ = 0;

    // superposiciones
    Dialog dialog_;
    TextView view_;
    std::string toastMsg_;
    ui::Color toastCol_ = ui::col::text;
    uint64_t toastUntil_ = 0;

    // barra inferior
    uint64_t freeSpace_ = 0;
    uint64_t lastStatus_ = 0;
    int64_t lastWall_ = 0;  // reloj de pared y reloj monotónico del fotograma anterior (detectan la suspensión)
    uint64_t lastMono_ = 0, lastSlowLog_ = 0;
    int lastFaults_ = 0;
    Async<uint64_t> spaceJob_;  // statvfs de la SD puede tardar: se mide en un hilo
    int importCount_ = 0;
    bool importDirty_ = true;
    uint64_t startedAt_ = 0;
    bool autoUpdChecked_ = false;
};
