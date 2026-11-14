// Tests del núcleo (sin SDL ni libnx). Compilar con tests/run.sh
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>

#include "../source/gamebanana.hpp"
#include "../source/installer.hpp"
#include "../source/json.hpp"
#include "../source/md5.hpp"
#include "../source/modstore.hpp"
#include "../source/segdl.hpp"
#include "../source/sys.hpp"
#include "../source/settings.hpp"
#include "../source/util.hpp"

extern std::map<std::string, std::string> g_canned;
extern std::vector<std::string> g_requested;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("  FALLO %s:%d  %s\n", __FILE__, __LINE__, #c);       \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

static void touch(const std::string& path, const std::string& data = "x") {
    util::mkdirs(util::dirName(path));
    util::writeFile(path, data);
}

static void testJson() {
    printf("json\n");
    json::Value v;
    std::string err;
    CHECK(json::parse("{\"a\":1,\"b\":[true,null,\"x\\u00e9\\ud83d\\ude00\\n\"],\"c\":{\"d\":-2.5e1}}", v, &err));
    CHECK(v["a"].i64() == 1);
    CHECK(v["b"].size() == 3);
    CHECK(v["b"].at(0).boolean());
    CHECK(v["b"].at(1).isNull());
    CHECK(v["b"].at(2).str() == "x\xC3\xA9\xF0\x9F\x98\x80\n");
    CHECK(v["c"]["d"].num() == -25.0);
    CHECK(v["zzz"]["y"].str("def") == "def");
    CHECK(!json::parse("{\"a\":", v, &err));
    CHECK(!json::parse("[1,2,]", v, &err));
    CHECK(!json::parse("{} x", v, &err));
    json::Value o = json::Value::Obj();
    o.set("k", json::Value::S("a\"b\\c\n"));
    o.set("n", json::Value::N(1700000000));
    json::Value back;
    CHECK(json::parse(o.dump(2), back));
    CHECK(back["k"].str() == "a\"b\\c\n");
    CHECK(back["n"].i64() == 1700000000);
    std::string deep(200, '[');
    CHECK(!json::parse(deep, v));
}

static void testMd5() {
    printf("md5\n");
    CHECK(md5::hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5::hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5::hex("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");
    std::string big(100000, 'a');
    util::writeFile("/tmp/md5_test.bin", big);
    CHECK(md5::file("/tmp/md5_test.bin") == md5::hex(big));
}

static void testUtil() {
    printf("util\n");
    CHECK(util::sanitizeName("My: Cool/Mod?*. ") == "My_ Cool_Mod__");
    CHECK(util::sanitizeName("...") == "mod");
    CHECK(util::sanitizeName(".hidden") == "hidden");
    CHECK(util::sanitizeName(std::string(300, 'a')).size() == 80);
    CHECK(util::stripHtml("<p>Hola &amp; <b>adiós</b></p><ul><li>uno</li><li>dos</li></ul>") == "Hola & adiós\n\n- uno\n- dos");
    CHECK(util::stripHtml("a<br>b&#233;&nbsp;c") == "a\nb\xC3\xA9 c");
    CHECK(util::formatDate(0) == "");
    CHECK(util::formatDate(1700000000) == "2023-11-14");
    CHECK(util::formatDate(951782400) == "2000-02-29");
    CHECK(util::urlEncode("a b&c/é") == "a%20b%26c%2F%C3%A9");
    CHECK(util::humanSize(1536) == "2 KB" || util::humanSize(1536) == "1 KB" || util::humanSize(1536) == "2 KB");
    CHECK(util::humanSize(5 * 1024 * 1024) == "5.0 MB");
    CHECK(util::baseName("/a/b/c/") == "c");
    CHECK(util::dirName("/a/b/c") == "/a/b");
}

static void testSafeRelative() {
    printf("rutas seguras (zip-slip)\n");
    std::string o;
    CHECK(safeRelative("Mod/fighter/mario/a.bin", o) && o == "Mod/fighter/mario/a.bin");
    CHECK(safeRelative("./Mod//ui/./x.bntx", o) && o == "Mod/ui/x.bntx");
    CHECK(safeRelative("/abs/olute/x.bin", o) && o == "abs/olute/x.bin");  // la raíz se descarta
    CHECK(safeRelative("C:\\Users\\x\\fighter\\a.bin", o) && o == "C_/Users/x/fighter/a.bin");
    CHECK(!safeRelative("../../atmosphere/evil.nro", o));
    CHECK(!safeRelative("Mod/../../evil", o));
    CHECK(!safeRelative("Mod\\..\\..\\evil", o));
    CHECK(!safeRelative("", o));
    CHECK(!safeRelative("/", o));
    CHECK(!safeRelative(nullptr, o));
    CHECK(safeRelative("Mod/a:b?.txt", o) && o == "Mod/a_b_.txt");
}

static void testGb() {
    printf("gamebanana\n");
    // Forma real de la API v11 (campos verificados): _aMetadata, _aRecords, _aPreviewMedia._aImages...
    const char* feed = R"({
      "_aMetadata": {"_nRecordCount": 3, "_bIsComplete": false, "_nPerpage": 20},
      "_aRecords": [
        {"_idRow": 11, "_sModelName": "Mod", "_sName": "Mario Recolor", "_sProfileUrl": "https://gamebanana.com/mods/11",
         "_tsDateAdded": 1700000000, "_tsDateModified": 1700001000, "_sVersion": "1.2",
         "_aPreviewMedia": {"_aImages": [{"_sType":"screenshot","_sBaseUrl":"https://images.gamebanana.com/img/ss/mods","_sFile":"abc.jpg","_sFile100":"100-90_abc.jpg","_sFile220":"220-90_abc.jpg","_sFile530":"530-90_abc.jpg"},
                                         {"_sType":"screenshot","_sBaseUrl":"https://images.gamebanana.com/img/ss/mods","_sFile":"def.png","_sFile100":"100-90_def.png"}]},
         "_aSubmitter": {"_idRow": 5, "_sName": "Autor"}, "_aGame": {"_idRow": 6498, "_sName": "Super Smash Bros. Ultimate"},
         "_aRootCategory": {"_sName": "Skins"}, "_aSubCategory": {"_sName": "Mario"}},
        {"_idRow": 12, "_sModelName": "Mod", "_sName": "Otro Juego", "_aGame": {"_idRow": 2, "_sName": "CS:S"}, "_aPreviewMedia": {"_aImages": []}},
        {"_idRow": 13, "_sModelName": "Wip", "_sName": "No es mod", "_aGame": {"_idRow": 6498}},
        {"_idRow": 14, "_sModelName": "Sound", "_sName": "Voz", "_tsDateUpdated": 1700005000, "_nLikeCount": 7, "_nViewCount": 99}
      ]})";
    json::Value v;
    CHECK(json::parse(feed, v));
    gb::Page p;
    gb::setGameId(6498);
    CHECK(gb::parseRecords(v, p));
    CHECK(p.items.size() == 2);
    CHECK(p.dropped == 2);
    CHECK(p.total == 3 && !p.complete);
    CHECK(p.items[0].name == "Mario Recolor");
    CHECK(p.items[0].author == "Autor");
    CHECK(p.items[0].category() == "Skins / Mario");
    CHECK(p.items[0].thumbUrl() == "https://images.gamebanana.com/img/ss/mods/220-90_abc.jpg");
    CHECK(p.items[0].galleryUrl(0) == "https://images.gamebanana.com/img/ss/mods/530-90_abc.jpg");
    CHECK(p.items[0].galleryUrl(1) == "https://images.gamebanana.com/img/ss/mods/def.png");
    CHECK(p.items[1].model == "Sound" && p.items[1].likes == 7 && p.items[1].views == 99 && p.items[1].updated == 1700005000);

    // Detalle + archivos
    g_canned.clear();
    g_canned["https://gamebanana.com/apiv11/Mod/11/ProfilePage"] = R"({
      "_idRow": 11, "_sName": "Mario Recolor", "_sText": "<p>Hola &amp; adi&oacute;s</p><br>Linea 2",
      "_nDownloadCount": 1234, "_aFiles": [
        {"_idRow": 100, "_sFile": "viejo.zip", "_nFilesize": 1000, "_tsDateAdded": 1600000000, "_sMd5Checksum": "ABCDEF0123456789ABCDEF0123456789"},
        {"_idRow": 200, "_sFile": "nuevo.7z", "_nFilesize": 2048, "_tsDateAdded": 1700000000, "_sDownloadUrl": "https://gamebanana.com/dl/200"}]})";
    gb::Item base = p.items[0];
    gb::Detail d;
    std::string err;
    CHECK(gb::detail(base, d, err));
    CHECK(d.files.size() == 2);
    CHECK(d.files[0].id == 200 && d.files[0].url == "https://gamebanana.com/dl/200");
    CHECK(d.files[1].url == "https://gamebanana.com/dl/100");
    CHECK(d.files[1].md5 == "abcdef0123456789abcdef0123456789");
    CHECK(d.downloads == 1234);
    CHECK(d.text.find("Hola & adi") == 0);
    CHECK(d.item.author == "Autor");  // conserva lo que ya se sabía
    CHECK(d.item.images.size() == 2);

    // Último archivo (actualizaciones)
    g_canned["https://gamebanana.com/apiv11/Mod/11?_csvProperties=_aFiles"] = R"({"_aFiles": [{"_idRow": 5, "_sFile":"a.zip"}, {"_idRow": 9, "_sFile":"b.zip"}]})";
    CHECK(gb::latestFileId("Mod", 11) == 9);

    // Navegación y plan B de la búsqueda
    g_canned.clear();
    g_canned["https://gamebanana.com/apiv11/Game/6498/Subfeed?_nPage=1"] = feed;
    g_requested.clear();
    gb::clearFeedCache();
    gb::Page s = gb::search("mario", 1, 20);
    CHECK(s.ok);
    CHECK(s.items.size() == 1 && s.items[0].id == 11);
    CHECK(g_requested.size() >= 2 && g_requested[0].find("Util/Search/Results") != std::string::npos);
    {  // segunda búsqueda: el feed sale de la caché (no hay nuevas peticiones a Subfeed)
        size_t before = g_requested.size();
        gb::Page s2 = gb::search("mario", 1, 20);
        CHECK(s2.ok && s2.items.size() == 1);
        int feedReqs = 0;
        for (size_t i = before; i < g_requested.size(); i++) feedReqs += g_requested[i].find("Subfeed?_nPage=1&") != std::string::npos ? 1 : 0;  // la página 1 ya está en caché
        CHECK(feedReqs == 0);
    }
    gb::Page b = gb::browse(1, gb::Sort::New, 20);
    CHECK(b.ok && b.items.size() == 2);
    CHECK(g_requested.back().find("_sSort=new") != std::string::npos);
    g_canned.clear();
    gb::Page bad = gb::browse(1, gb::Sort::Default, 20);
    CHECK(!bad.ok && !bad.err.empty());
}

static void testInstallerAndStore() {
    printf("instalador + modstore\n");
    std::string root = "/tmp/smh_test_root";
    util::rmrf(root);
    util::mkdirs(root);
    paths::setRoot(root);
    util::mkdirs(paths::data());

    // 1) zip con carpeta contenedora
    std::string t1 = paths::tmp() + "/t1";
    touch(t1 + "/Mario Skin/fighter/mario/model/body/c01/model.nutexb");
    touch(t1 + "/Mario Skin/ui/replace/chara/chara_1_mario_00.bntx");
    touch(t1 + "/Mario Skin/preview.webp");
    touch(t1 + "/__MACOSX/._junk");
    InstallResult r1 = installFromDir(t1, "Fallback", paths::mods(), "");
    CHECK(r1.ok && r1.folders.size() == 1 && r1.folders[0] == "Mario Skin");
    CHECK(util::isFile(paths::mods() + "/Mario Skin/fighter/mario/model/body/c01/model.nutexb"));

    // 2) zip sin carpeta contenedora -> usa el nombre de respaldo
    std::string t2 = paths::tmp() + "/t2";
    touch(t2 + "/fighter/luigi/model/body/c01/model.nutexb");
    touch(t2 + "/config.json");
    InstallResult r2 = installFromDir(t2, "Luigi: Verde?", paths::mods(), "");
    CHECK(r2.ok && r2.folders.size() == 1 && r2.folders[0] == "Luigi_ Verde_");

    // 3) paquete con varios mods + ultimate/mods anidado
    std::string t3 = paths::tmp() + "/t3";
    touch(t3 + "/Pack/ultimate/mods/Peach A/fighter/peach/x.bin");
    touch(t3 + "/Pack/ultimate/mods/Peach B/sound/bgm/y.nus3audio");
    touch(t3 + "/Pack/atmosphere/contents/01006A800016E000/romfs/skyline/plugins/libx.nro");
    InstallResult r3 = installFromDir(t3, "Pack", paths::mods(), "");
    CHECK(r3.ok && r3.folders.size() == 2);
    CHECK(util::isDir(paths::mods() + "/Peach A") && util::isDir(paths::mods() + "/Peach B"));
    CHECK(!util::exists(paths::mods() + "/Pack"));

    // 4) nombre repetido -> sufijo (2)
    std::string t4 = paths::tmp() + "/t4";
    touch(t4 + "/Mario Skin/fighter/mario/other.bin");
    InstallResult r4 = installFromDir(t4, "x", paths::mods(), "");
    CHECK(r4.ok && r4.folders[0] == "Mario Skin (2)");

    // 5) actualización: reemplaza la carpeta existente (y no deja copias de seguridad sueltas)
    std::string t5 = paths::tmp() + "/t5";
    touch(t5 + "/Cualquier Nombre/fighter/mario/new.bin");
    InstallResult r5 = installFromDir(t5, "x", paths::mods(), "Mario Skin");
    CHECK(r5.ok && r5.folders[0] == "Mario Skin");
    CHECK(util::isFile(paths::mods() + "/Mario Skin/fighter/mario/new.bin"));
    CHECK(!util::exists(paths::mods() + "/Mario Skin/fighter/mario/model/body/c01/model.nutexb"));
    for (const auto& n : util::listDirs(paths::tmp())) CHECK(!util::startsWith(n, "replaced_"));

    // 6) sin contenido de mod -> error
    std::string t6 = paths::tmp() + "/t6";
    touch(t6 + "/atmosphere/contents/x/exefs/subsdk9");
    touch(t6 + "/readme.txt");
    InstallResult r6 = installFromDir(t6, "x", paths::mods(), "");
    CHECK(!r6.ok && !r6.err.empty());

    // ModStore
    ModStore st;
    st.load();
    CHECK(st.mods().size() == 5);  // Mario Skin, Mario Skin (2), Luigi_ Verde_, Peach A, Peach B
    Meta m;
    m.gbId = 11;
    m.model = "Mod";
    m.name = "Mario Recolor";
    m.fileId = 100;
    m.latestFileId = 100;
    st.recordInstall("Mario Skin", m);
    st.load();
    int mi = st.findByGb(11);
    CHECK(mi >= 0 && st.mods()[(size_t)mi].folder == "Mario Skin");
    CHECK(st.mods()[(size_t)mi].title() == "Mario Recolor");
    CHECK(!st.mods()[(size_t)mi].hasUpdate());
    st.setLatest(11, 200);
    CHECK(st.mods()[(size_t)mi].hasUpdate());

    std::string err;
    CHECK(st.setEnabled((size_t)mi, false, err));
    CHECK(util::isDir(paths::modsDisabled() + "/Mario Skin") && !util::exists(paths::mods() + "/Mario Skin"));
    st.load();
    mi = st.findByGb(11);
    CHECK(mi >= 0 && !st.mods()[(size_t)mi].enabled);
    CHECK(st.mods()[(size_t)mi].meta.fileId == 100);  // el db sobrevive a recargar

    // Perfiles
    st.saveProfile("solo-peach");  // (en este momento: Mario Skin desactivado, resto activados)
    std::string e2;
    CHECK(st.setAll(false, e2) >= 1);
    CHECK(st.enabledCount() == 0);
    int changed = st.applyProfile("solo-peach", e2);
    CHECK(changed == (int)st.mods().size() - 1);
    CHECK(st.enabledCount() == (int)st.mods().size() - 1);
    CHECK(st.profileNames().size() == 1);

    // Conflictos
    touch(paths::mods() + "/Peach A/fighter/peach/shared.bin");
    touch(paths::mods() + "/Peach B/fighter/peach/SHARED.bin");  // distinto caso: en FAT es el mismo archivo lógico
    touch(paths::mods() + "/Peach A/config.json");
    touch(paths::mods() + "/Peach B/config.json");  // en la raíz: no cuenta
    std::vector<std::string> enabled;
    for (auto& mm : st.mods())
        if (mm.enabled) enabled.push_back(mm.folder);
    std::atomic<int> done{0};
    auto conflicts = scanConflicts(enabled, &done, nullptr);
    CHECK(conflicts.size() == 1);
    if (!conflicts.empty()) {
        CHECK(conflicts[0].path == "fighter/peach/shared.bin");
        CHECK(conflicts[0].mods.size() == 2);
    }
    CHECK(done.load() == (int)enabled.size());

    // Borrado
    int pi = st.findByFolder("Peach A");
    CHECK(pi >= 0 && st.remove((size_t)pi, err));
    CHECK(!util::exists(paths::mods() + "/Peach A"));
    st.load();
    CHECK(st.findByFolder("Peach A") < 0);

    // Persistencia de ajustes
    g_settings.english = true;
    g_settings.perPage = 30;
    g_settings.save();
    Settings s2;
    s2.load();
    CHECK(s2.english && s2.perPage == 30);
    g_settings = Settings();
}


// ---------------------------------------------------------------- descarga por trozos
namespace {
struct FakeTransport : http::Transport {
    std::string data;
    bool ranges = true;
    bool probeOk = true;
    bool ignoreRange = false;  // responde "no soporto Range" al pedir un trozo
    int delayMs = 0;
    std::atomic<int> failFirst{0};  // los primeros N intentos (de cualquier hilo) fallan
    int alwaysFailChunk = -1;       // el trozo con este índice falla siempre
    uint64_t chunk = 1 << 20;
    std::atomic<int> calls{0}, cur{0}, maxConc{0};
    std::atomic<uint64_t> usedEff{0}, usedOrig{0};
    std::string effUrl = "https://cdn.example/final";
    std::string effBad;  // si no está vacío, esa URL devuelve error siempre

    http::Probe probe(const std::string&) override {
        http::Probe p;
        p.ok = probeOk;
        p.ranges = ranges;
        p.size = data.size();
        p.url = effUrl;
        p.status = 206;
        return p;
    }
    http::FetchResult fetchRange(const std::string& url, uint64_t from, uint64_t to, std::string& out, const std::atomic<bool>& cancel, std::atomic<uint64_t>& received) override {
        http::FetchResult r;
        calls++;
        int c = ++cur;
        int m = maxConc.load();
        while (c > m && !maxConc.compare_exchange_weak(m, c)) {
        }
        struct Dec { std::atomic<int>& x; ~Dec() { x--; } } dec{cur};
        (url == effUrl ? usedEff : usedOrig)++;
        if (ignoreRange) {
            r.st = http::Fetch::Unsupported;
            return r;
        }
        // Tarda distinto según el trozo para que terminen desordenados; reparte en pasos para poder cancelar.
        int ms = delayMs ? (int)((from / chunk * 7) % (uint64_t)delayMs) + 1 : 0;
        for (int i = 0; i < ms; i++) {
            if (cancel.load()) {
                r.st = http::Fetch::Cancelled;
                return r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (cancel.load()) {
            r.st = http::Fetch::Cancelled;
            return r;
        }
        if (!effBad.empty() && url == effBad) {
            r.st = http::Fetch::Retry;
            r.error = "HTTP 403";
            return r;
        }
        if (failFirst.load() > 0 && failFirst.fetch_sub(1) > 0) {
            r.st = http::Fetch::Retry;
            r.error = "boom";
            return r;
        }
        if (alwaysFailChunk >= 0 && from / chunk == (uint64_t)alwaysFailChunk) {
            r.st = http::Fetch::Retry;
            r.error = "siempre falla";
            return r;
        }
        out.append(data, (size_t)from, (size_t)(to - from + 1));
        received += to - from + 1;
        return r;
    }
};

std::string makeData(size_t n) {
    std::string s(n, 0);
    uint32_t x = 12345;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        s[i] = (char)(x >> 24);
    }
    return s;
}

// Ejecuta una descarga y devuelve lo escrito en el archivo.
http::SegResult runSeg(FakeTransport& ft, http::SegOptions opt, std::string& written, Progress* prog = nullptr) {
    FILE* f = tmpfile();
    http::Probe p = ft.probe("https://x/dl");
    http::SegResult r = http::segmentedDownload(ft, "https://x/dl", p, f, prog, opt);
    fflush(f);
    long n = ftell(f);
    rewind(f);
    written.assign((size_t)(n > 0 ? n : 0), 0);
    if (n > 0) CHECK(fread(&written[0], 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return r;
}
}  // namespace

static void testSegdl() {
    printf("segdl\n");
    for (size_t size : {(size_t)5500000, (size_t)(4u << 20), (size_t)(1u << 20) + 1, (size_t)1}) {
        for (int conns : {1, 4, 6}) {
            FakeTransport ft;
            ft.data = makeData(size);
            ft.delayMs = 6;
            http::SegOptions o;
            o.conns = conns;
            std::string got;
            Progress pr;
            http::SegResult r = runSeg(ft, o, got, &pr);
            CHECK(r.st == http::SegStatus::Done);
            CHECK(got == ft.data);
            CHECK(pr.now.load() == size && pr.total.load() == size);
            CHECK(ft.maxConc.load() <= conns);
        }
    }
    {  // muchos trozos pequeños con ventana mínima: todo en orden y sin bloquearse
        FakeTransport ft;
        ft.data = makeData(3000000);
        ft.chunk = 64 * 1024;
        ft.delayMs = 5;
        http::SegOptions o;
        o.conns = 4;
        o.chunk = 64 * 1024;
        o.window = 2;
        std::string got;
        http::SegResult r = runSeg(ft, o, got);
        CHECK(r.st == http::SegStatus::Done);
        CHECK(got == ft.data);
        CHECK(ft.calls.load() == (int)((3000000 + 65535) / 65536));
    }
    {  // fallos pasajeros: se reintentan
        FakeTransport ft;
        ft.data = makeData(6u << 20);
        ft.failFirst = 3;
        http::SegOptions o;
        std::string got;
        http::SegResult r = runSeg(ft, o, got);
        CHECK(r.st == http::SegStatus::Done);
        CHECK(got == ft.data);
        CHECK(ft.calls.load() == 6 + 3);
    }
    {  // la URL final falla (403): pasa a la original
        FakeTransport ft;
        ft.data = makeData(3u << 20);
        ft.effBad = ft.effUrl;
        http::SegOptions o;
        std::string got;
        http::SegResult r = runSeg(ft, o, got);
        CHECK(r.st == http::SegStatus::Done);
        CHECK(got == ft.data);
        CHECK(ft.usedOrig.load() >= 3);
    }
    {  // un trozo falla siempre: error, y lo escrito es un prefijo correcto
        FakeTransport ft;
        ft.data = makeData(8u << 20);
        ft.alwaysFailChunk = 3;
        http::SegOptions o;
        o.attempts = 2;
        std::string got;
        http::SegResult r = runSeg(ft, o, got);
        CHECK(r.st == http::SegStatus::Failed);
        CHECK(!r.error.empty() && !r.diskError);
        CHECK(got.size() <= (size_t)(3u << 20));
        CHECK(ft.data.compare(0, got.size(), got) == 0);
    }
    {  // el servidor no sirve trozos
        FakeTransport ft;
        ft.data = makeData(3u << 20);
        ft.ignoreRange = true;
        std::string got;
        http::SegResult r = runSeg(ft, http::SegOptions(), got);
        CHECK(r.st == http::SegStatus::Unsupported);
    }
    {  // la sonda no vale / no admite rangos: ni se intenta
        FakeTransport ft;
        ft.data = makeData(3u << 20);
        ft.ranges = false;
        std::string got;
        http::SegResult r = runSeg(ft, http::SegOptions(), got);
        CHECK(r.st == http::SegStatus::Unsupported);
        CHECK(ft.calls.load() == 0);
    }
    {  // cancelar a mitad
        FakeTransport ft;
        ft.data = makeData(20u << 20);
        ft.delayMs = 30;
        Progress pr;
        std::thread th([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            pr.cancel = true;
        });
        std::string got;
        uint64_t t0 = sys::nowMs();
        http::SegResult r = runSeg(ft, http::SegOptions(), got, &pr);
        th.join();
        CHECK(r.st == http::SegStatus::Cancelled);
        CHECK(sys::nowMs() - t0 < 3000);
    }
    {  // disco lleno
        FakeTransport ft;
        ft.data = makeData(3u << 20);
        FILE* f = fopen("/dev/full", "wb");
        if (f) {
            setvbuf(f, nullptr, _IONBF, 0);
            http::Probe p = ft.probe("u");
            http::SegResult r = http::segmentedDownload(ft, "u", p, f, nullptr, http::SegOptions());
            fclose(f);
            CHECK(r.st == http::SegStatus::Failed && r.diskError);
        }
    }
    {  // velocidad suavizada
        SpeedMeter m;
        CHECK(m.update(1000, 0) == 0);
        CHECK(m.update(1200, 1000) == 0);  // ventana < 400 ms: sin cambios
        uint64_t b = m.update(1500, 500000);
        CHECK(b > 900000 && b < 1100000);  // ~1 MB/s
        uint64_t b2 = m.update(2000, 500000);
        CHECK(b2 < b);  // se frena y baja gradualmente
    }
}

int main() {
    testJson();
    testMd5();
    testUtil();
    testSafeRelative();
    testGb();
    testInstallerAndStore();
    testSegdl();
    printf(g_fail ? "\n%d FALLOS\n" : "\nTODO OK\n", g_fail);
    return g_fail ? 1 : 0;
}
