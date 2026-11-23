// Tests del núcleo (sin SDL ni libnx). Compilar con tests/run.sh
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
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
#include "../source/reslot.hpp"
#include "../source/segdl.hpp"
#include "../source/sys.hpp"
#include "../source/settings.hpp"
#include "../source/util.hpp"

extern std::map<std::string, std::string> g_canned;
extern std::function<void(const std::string&)> g_onGet;
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
    {
        md5::Hasher h;
        for (size_t i = 0; i < big.size(); i += 777) h.update(big.data() + i, std::min<size_t>(777, big.size() - i));
        CHECK(h.finish() == md5::hex(big));
    }
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
    {  // el buscador contesta bien pero sin resultados: la página 2 NO repite la búsqueda local (devolvía la página 1 otra vez)
        g_canned.clear();
        gb::clearFeedCache();
        g_canned["https://gamebanana.com/apiv11/Util/Search/Results"] = R"({"_aMetadata":{"_bIsComplete":true},"_aRecords":[]})";
        g_canned["https://gamebanana.com/apiv11/Game/6498/Subfeed?_nPage=1"] = feed;
        g_requested.clear();
        gb::Page e2 = gb::search("zzz", 2, 20);
        CHECK(e2.ok && e2.items.empty() && e2.complete);
        CHECK(g_requested.size() == 1);
        // la página 1 sí mira lo más reciente (como mucho 2 páginas) y avisa del progreso
        g_requested.clear();
        gb::Page e1 = gb::search("mario", 1, 20);
        CHECK(e1.ok && e1.items.size() == 1 && e1.scanned > 0);
        int d = -1, t = -1;
        gb::searchProgress(d, t);
        CHECK(t == 2 && d >= 1 && d <= 2);
        int feedReqs = 0;
        for (const auto& u : g_requested) feedReqs += u.find("Subfeed") != std::string::npos ? 1 : 0;
        CHECK(feedReqs <= 2);
        // pedir otra cosa cancela la búsqueda larga en curso
        gb::clearFeedCache();
        g_canned.clear();  // el buscador falla -> plan B de 4 páginas
        g_canned["https://gamebanana.com/apiv11/Game/6498/Subfeed?_nPage=1"] = feed;
        g_onGet = [](const std::string& u) {
            if (u.find("Subfeed") != std::string::npos) gb::cancelSearch();
        };
        gb::Page cx = gb::search("mario", 1, 20);
        g_onGet = nullptr;
        CHECK(!cx.ok && cx.err == "cancelada");
        gb::clearFeedCache();
        gb::Page again = gb::search("mario", 1, 20);  // y la siguiente búsqueda funciona con normalidad
        CHECK(again.ok && again.items.size() == 1);
        g_canned.clear();
        g_canned["https://gamebanana.com/apiv11/Game/6498/Subfeed?_nPage=1"] = feed;
    }
    gb::Page b = gb::browse(1, gb::Sort::New, 20);
    CHECK(b.ok && b.items.size() == 2);
    CHECK(g_requested.back().find("_sSort=new") != std::string::npos);
    g_canned.clear();
    gb::Page bad = gb::browse(1, gb::Sort::Default, 20);
    CHECK(!bad.ok && !bad.err.empty());
}


static void testCategories() {
    printf("gamebanana: categorías y filtros\n");
    const std::string api = "https://gamebanana.com/apiv11";
    paths::setRoot("/tmp/smh_test_cat");
    util::rmrf("/tmp/smh_test_cat");
    util::mkdirs(paths::cache());
    gb::setGameId(6498);

    // parseCategories: forma de Mod/Categories
    {
        json::Value v;
        CHECK(json::parse(R"({"_aMetadata":{"_nRecordCount":3,"_bIsComplete":true},"_aRecords":[
            {"_idRow":100,"_sModelName":"ModCategory","_sName":"Skins","_sProfileUrl":"https://gamebanana.com/mods/cats/100","_nItemCount":50},
            {"_sName":"Final Smashes","_sProfileUrl":"https://gamebanana.com/mods/cats/101"},
            {"_idRow":100,"_sName":"Duplicada"},
            {"_idRow":0,"_sName":"Sin id"}]})", v));
        std::vector<gb::Category> out;
        bool complete = false;
        CHECK(gb::parseCategories(v, out, &complete));
        CHECK(complete);
        CHECK(out.size() == 2);
        CHECK(out[0].id == 100 && out[0].name == "Skins" && out[0].count == 50);
        CHECK(out[1].id == 101 && out[1].name == "Final Smashes" && out[1].count == -1);
        // si los registros son mods, no son categorías
        json::Value m;
        CHECK(json::parse(R"({"_aRecords":[{"_idRow":5,"_sModelName":"Mod","_sName":"x"}]})", m));
        std::vector<gb::Category> none;
        CHECK(!gb::parseCategories(m, none));
        CHECK(gb::catIdFromUrl("https://gamebanana.com/mods/cats/12345") == 12345);
        CHECK(gb::catIdFromUrl("https://gamebanana.com/mods/11") == 0);
    }

    const char* roots = R"({"_aMetadata":{"_nRecordCount":2,"_bIsComplete":true},"_aRecords":[
        {"_idRow":100,"_sModelName":"ModCategory","_sName":"Skins","_sProfileUrl":"https://gamebanana.com/mods/cats/100"},
        {"_idRow":101,"_sModelName":"ModCategory","_sName":"Final Smashes","_sProfileUrl":"https://gamebanana.com/mods/cats/101"}]})";
    const char* kids = R"({"_aMetadata":{"_nRecordCount":2,"_bIsComplete":true},"_aRecords":[
        {"_idRow":200,"_sModelName":"ModCategory","_sName":"Mario","_nItemCount":12},
        {"_idRow":201,"_sModelName":"ModCategory","_sName":"Link","_nItemCount":7}]})";

    // listado de la raíz: se prueba la primera variante, se guarda en caché y no se vuelve a pedir
    g_canned.clear();
    g_canned[api + "/Mod/Categories?_idGameRow=6498&_sSort=a_to_z&_nPage=1&_nPerpage=50$"] = roots;
    g_requested.clear();
    gb::CategoryList r0 = gb::categories(0);
    CHECK(r0.ok && !r0.learned && !r0.stale && r0.cats.size() == 2 && r0.cats[0].name == "Skins");
    size_t n1 = g_requested.size();
    gb::CategoryList r0b = gb::categories(0);
    CHECK(r0b.ok && r0b.cats.size() == 2);
    CHECK(g_requested.size() == n1);  // desde la caché
    CHECK(util::isFile(util::join(paths::cache(), "categories.json")));

    // la caché del disco se recupera tras "reiniciar"
    gb::setGameId(6498);  // vacía la memoria
    g_canned.clear();     // sin red
    gb::loadCategoryCache();
    gb::CategoryList r0c = gb::categories(0);
    CHECK(r0c.ok && r0c.cats.size() == 2);

    // hijos: variante 1 rechazada (404) y variante 2 válida
    g_canned.clear();
    g_canned[api + "/Mod/Categories?_idCategoryRow=100&_nPage=1&_nPerpage=50$"] = kids;
    gb::CategoryList k = gb::categories(100);
    CHECK(k.ok && k.cats.size() == 2 && k.cats[0].id == 200 && k.cats[0].parent == 100);

    // hoja: respuesta válida y vacía = sin subcategorías
    g_canned.clear();
    g_canned[api + "/Mod/Categories?_idCategoryRow=200&_sSort=a_to_z&_nPage=1&_nPerpage=50$"] = R"({"_aMetadata":{"_bIsComplete":true},"_aRecords":[]})";
    gb::CategoryList leaf = gb::categories(200);
    CHECK(leaf.ok && leaf.cats.empty());

    // el servidor ignora _idCategoryRow y devuelve las raíces: se rechaza (no se muestran como hijos)
    g_canned.clear();
    g_canned[api + "/Mod/Categories?_idCategoryRow=101"] = roots;
    gb::CategoryList bogus = gb::categories(101);
    CHECK(!bogus.ok || bogus.cats.empty());

    // sin respuesta de la API: se usan las categorías vistas en los mods ya cargados
    gb::setGameId(6498);
    util::rmrf(util::join(paths::cache(), "categories.json"));
    g_canned.clear();
    const char* feed = R"({"_aMetadata":{"_nRecordCount":2,"_bIsComplete":true},"_aRecords":[
      {"_idRow":1,"_sModelName":"Mod","_sName":"Mario Rojo","_aGame":{"_idRow":6498},
       "_aRootCategory":{"_sName":"Skins","_sProfileUrl":"https://gamebanana.com/mods/cats/100"},
       "_aSubCategory":{"_sName":"Mario","_sProfileUrl":"https://gamebanana.com/mods/cats/200"}},
      {"_idRow":2,"_sModelName":"Mod","_sName":"Nube","_aGame":{"_idRow":6498},
       "_aRootCategory":{"_sName":"Skins","_sProfileUrl":"https://gamebanana.com/mods/cats/100"},
       "_aSubCategory":{"_sName":"Cloud","_sProfileUrl":"https://gamebanana.com/mods/cats/201"}}]})";
    g_canned[api + "/Game/6498/Subfeed?_nPage=1"] = feed;
    gb::Page pf = gb::browse(1, gb::Sort::Default, 20);
    CHECK(pf.ok && pf.items.size() == 2 && pf.items[0].rootCatId == 100 && pf.items[0].subCatId == 200);
    g_canned.clear();
    gb::CategoryList lr = gb::categories(0);
    CHECK(lr.ok && lr.learned && lr.cats.size() == 1 && lr.cats[0].name == "Skins");
    gb::CategoryList lc = gb::categories(100);
    CHECK(lc.ok && lc.learned && lc.cats.size() == 2 && lc.cats[0].name == "Cloud");  // ordenadas por nombre
    gb::CategoryList nothing = gb::categories(999);
    CHECK(!nothing.ok && !nothing.err.empty());

    // mods de una categoría
    const char* idx = R"({"_aMetadata":{"_nRecordCount":2,"_bIsComplete":true},"_aRecords":[
      {"_idRow":21,"_sModelName":"Mod","_sName":"Cloud Azul","_aSubmitter":{"_sName":"Ana"},"_aGame":{"_idRow":6498}},
      {"_idRow":22,"_sModelName":"Mod","_sName":"Cloud Rojo","_aSubmitter":{"_sName":"Beto"},"_aGame":{"_idRow":6498}}]})";
    const std::string base = api + "/Mod/Index?_nPage=1&_nPerpage=20&_aFilters%5BGeneric_Category%5D=201";
    g_canned.clear();
    g_canned[base + "$"] = idx;
    g_requested.clear();
    gb::Page c1 = gb::browseCategory(201, 1, gb::Sort::Default, 20);
    CHECK(c1.ok && c1.items.size() == 2 && g_requested.size() == 1);
    // orden "Nuevos": el servidor acepta Generic_Newest
    g_canned.clear();
    g_canned[base + "&_sSort=Generic_Newest$"] = idx;
    CHECK(gb::browseCategory(201, 1, gb::Sort::New, 20).ok);
    // orden "Actualizados": el servidor rechaza ambos valores -> se pide sin orden (y se recuerda)
    g_canned.clear();
    g_canned[base + "$"] = idx;
    g_requested.clear();
    gb::Page c2 = gb::browseCategory(201, 1, gb::Sort::Updated, 20);
    CHECK(c2.ok && c2.items.size() == 2);
    CHECK(g_requested.size() == 3);  // 2 variantes rechazadas + sin orden
    g_requested.clear();
    CHECK(gb::browseCategory(201, 1, gb::Sort::Updated, 20).ok);
    CHECK(g_requested.size() == 1);  // ya se sabe que no hay orden válido
    // fallo de red (no 4xx): no se prueban más variantes
    g_canned.clear();
    g_requested.clear();
    gb::Page c3 = gb::browseCategory(201, 1, gb::Sort::Default, 20);
    CHECK(!c3.ok && g_requested.size() == 1);

    // búsqueda de texto dentro de la categoría (filtrado en local, con caché)
    gb::clearFeedCache();
    g_canned.clear();
    g_canned[api + "/Mod/Index?_nPage=1&_nPerpage=50&_aFilters%5BGeneric_Category%5D=201$"] = idx;
    gb::Page sc = gb::searchInCategory(201, "rojo");
    CHECK(sc.ok && sc.items.size() == 1 && sc.items[0].id == 22 && sc.scanned == 2);
    g_requested.clear();
    gb::Page sc2 = gb::searchInCategory(201, "ana");  // también por autor, y desde la caché
    CHECK(sc2.ok && sc2.items.size() == 1 && sc2.items[0].id == 21 && g_requested.empty());
    g_canned.clear();
    gb::clearFeedCache();
    CHECK(!gb::searchInCategory(201, "x").ok);
    gb::setGameId(6498);
}

// Lista ordenada de todas las rutas (relativas) de un árbol, para comparar antes/después.
static void snapshotRec(const std::string& base, const std::string& rel, std::vector<std::string>& out) {
    for (const auto& d : util::listDirs(util::join(base, rel))) {
        out.push_back(rel + d + "/");
        snapshotRec(base, rel + d + "/", out);
    }
    for (const auto& f : util::listFiles(util::join(base, rel))) out.push_back(rel + f);
}
static std::string snapshot(const std::string& base) {
    std::vector<std::string> v;
    snapshotRec(base, "", v);
    std::sort(v.begin(), v.end());
    std::string s;
    for (const auto& x : v) s += x + "\n";
    return s;
}
static bool has(const std::string& base, const std::string& rel) { return util::exists(util::join(base, rel)); }

static void buildMario(const std::string& m, const std::string& slot /* "c00" */, const std::string& num /* "00" */) {
    touch(m + "/fighter/mario/model/body/" + slot + "/model.nutexb", "body-" + slot);
    touch(m + "/fighter/mario/model/hair/" + slot + "/h.nutexb");
    touch(m + "/fighter/mario/motion/body/" + slot + "/a.nuanmb");
    touch(m + "/fighter/kirby/model/copy_mario_cap/" + slot + "/k.nutexb");
    touch(m + "/camera/fighter/mario/" + slot + "/cam.bin");
    touch(m + "/effect/fighter/mario/ef_mario_" + slot + ".eff");
    touch(m + "/effect/fighter/mario/trail_" + slot + "/t.bin");
    touch(m + "/effect/fighter/mario/model/fire_" + slot + "/c00/inner.eff");
    touch(m + "/sound/bank/fighter/se_mario_" + slot + ".nus3audio");
    touch(m + "/sound/bank/fighter_voice/vc_mario_" + slot + ".nus3audio");
    touch(m + "/ui/replace/chara/chara_0/chara_0_mario_" + num + ".bntx");
    touch(m + "/ui/replace/chara/chara_1/chara_1_mario_" + num + ".bntx");
}

static void testReslot() {
    printf("reslotter\n");
    const std::string root = "/tmp/smh_test_reslot";
    util::rmrf(root);
    util::mkdirs(root);
    const std::string journal = root + "/journal.json";

    CHECK(reslot::renamedWithSlot("c02", 5) == "c05");
    CHECK(reslot::renamedWithSlot("C02", 5) == "C05");
    CHECK(reslot::renamedWithSlot("ef_mario_c01.eff", 3) == "ef_mario_c03.eff");
    CHECK(reslot::renamedWithSlot("vc_ptrainer_low_c00.nus3audio", 7) == "vc_ptrainer_low_c07.nus3audio");
    CHECK(reslot::renamedWithSlot("chara_3_mario_02.bntx", 6) == "chara_3_mario_06.bntx");
    CHECK(reslot::renamedWithSlot("model.nutexb", 1).empty());
    CHECK(reslot::renamedWithSlot("c0", 1).empty());
    CHECK(reslot::renamedWithSlot("abc00", 1).empty());
    CHECK(reslot::configMentionsSlots(R"({"new-dir-files":{"fighter/mario/model/body/c00":[]}})"));
    CHECK(reslot::configMentionsSlots(R"({"x":"chara_0_mario_02.bntx"})"));
    CHECK(!reslot::configMentionsSlots("{}"));
    CHECK(!reslot::configMentionsSlots(R"({"share-to-vanilla":{"a":"b"}, "name":"abc00x"})"));

    // --- análisis
    const std::string m1 = root + "/mario";
    buildMario(m1, "c00", "00");
    touch(m1 + "/item/thing/c00/skip.nutexb");  // item/: no se toca
    touch(m1 + "/fighter/mario/param/vl_c00.prc");  // con pinta de slot pero no reconocido
    reslot::Analysis a = reslot::analyze(m1);
    CHECK(a.ok());
    CHECK(a.perFighter.size() == 1 && a.perFighter.count("mario") == 1);
    CHECK(a.slots.size() == 1 && a.slots[0] == 0);
    CHECK(a.items == 12);
    CHECK(a.unknown.size() == 1);
    CHECK(reslot::fighterSlots(m1).at("mario").size() == 1);

    // --- reslotear c00 -> c03
    const std::string before = snapshot(m1);
    reslot::Plan plan;
    CHECK(reslot::makePlan(m1, {{0, 3}}, plan) == reslot::Problem::None);
    CHECK(plan.items == 12 && plan.ops.size() == 24);
    CHECK(snapshot(m1) == before);  // preparar el plan no toca nada
    std::string err;
    std::atomic<int> prog{0};
    CHECK(reslot::apply(plan, journal, &prog, nullptr, err) == reslot::Problem::None);
    CHECK(prog.load() == 24);
    CHECK(!util::exists(journal));
    CHECK(has(m1, "fighter/mario/model/body/c03/model.nutexb") && !has(m1, "fighter/mario/model/body/c00"));
    CHECK(has(m1, "fighter/mario/model/hair/c03/h.nutexb"));
    CHECK(has(m1, "fighter/mario/motion/body/c03/a.nuanmb"));
    CHECK(has(m1, "fighter/kirby/model/copy_mario_cap/c03/k.nutexb"));
    CHECK(has(m1, "camera/fighter/mario/c03/cam.bin"));
    CHECK(has(m1, "effect/fighter/mario/ef_mario_c03.eff") && has(m1, "effect/fighter/mario/trail_c03/t.bin"));
    CHECK(has(m1, "effect/fighter/mario/model/fire_c03/c00/inner.eff"));  // la c00 interna se queda
    CHECK(has(m1, "sound/bank/fighter/se_mario_c03.nus3audio") && has(m1, "sound/bank/fighter_voice/vc_mario_c03.nus3audio"));
    CHECK(has(m1, "ui/replace/chara/chara_0/chara_0_mario_03.bntx") && has(m1, "ui/replace/chara/chara_1/chara_1_mario_03.bntx"));
    CHECK(has(m1, "item/thing/c00/skip.nutexb"));
    CHECK(reslot::analyze(m1).slots == std::vector<int>{3});

    // --- intercambio c00 <-> c01 (necesita nombres temporales)
    const std::string m2 = root + "/luigi";
    touch(m2 + "/fighter/luigi/model/body/c00/a.nutexb", "AAA");
    touch(m2 + "/fighter/luigi/model/body/c01/a.nutexb", "BBB");
    touch(m2 + "/sound/bank/fighter_voice/vc_luigi_c01.nus3audio", "VOZ1");
    CHECK(reslot::analyze(m2).slots == (std::vector<int>{0, 1}));
    CHECK(reslot::makePlan(m2, {{0, 0}, {1, 0}}, plan) == reslot::Problem::BadMap);  // dos slots al mismo destino
    CHECK(reslot::makePlan(m2, {{0, 1}}, plan) == reslot::Problem::BadMap);          // c01 existente quedaría repetido
    CHECK(reslot::makePlan(m2, {{0, 9}}, plan) == reslot::Problem::BadMap);          // fuera de rango
    CHECK(reslot::makePlan(m2, {{0, 1}, {1, 0}}, plan) == reslot::Problem::None);
    CHECK(reslot::apply(plan, journal, nullptr, nullptr, err) == reslot::Problem::None);
    std::string c;
    CHECK(util::readFile(m2 + "/fighter/luigi/model/body/c00/a.nutexb", c) && c == "BBB");
    CHECK(util::readFile(m2 + "/fighter/luigi/model/body/c01/a.nutexb", c) && c == "AAA");
    CHECK(has(m2, "sound/bank/fighter_voice/vc_luigi_c00.nus3audio") && !has(m2, "sound/bank/fighter_voice/vc_luigi_c01.nus3audio"));
    // desplazar todo +1 (c00->c01, c01->c02)
    CHECK(reslot::makePlan(m2, {{0, 1}, {1, 2}}, plan) == reslot::Problem::None);
    CHECK(reslot::apply(plan, journal, nullptr, nullptr, err) == reslot::Problem::None);
    CHECK(has(m2, "fighter/luigi/model/body/c01") && has(m2, "fighter/luigi/model/body/c02") && !has(m2, "fighter/luigi/model/body/c00"));
    CHECK(reslot::makePlan(m2, {}, plan) == reslot::Problem::NoSlots);  // nada que cambiar

    // --- casos que se rechazan
    const std::string m3 = root + "/conconfig";
    touch(m3 + "/fighter/mario/model/body/c00/a");
    touch(m3 + "/config.json", R"({"new-dir-files":{"fighter/mario/model/body/c08":[]}})");
    CHECK(reslot::analyze(m3).problem == reslot::Problem::HasConfig);
    CHECK(reslot::makePlan(m3, {{0, 1}}, plan) == reslot::Problem::HasConfig);
    touch(m3 + "/config.json", "{}");  // config.json sin slots: no estorba
    CHECK(reslot::makePlan(m3, {{0, 1}}, plan) == reslot::Problem::None);
    const std::string m4 = root + "/extra";
    touch(m4 + "/fighter/mario/model/body/c08/a");
    CHECK(reslot::analyze(m4).problem == reslot::Problem::ExtraSlots);
    const std::string m5 = root + "/sinslots";
    touch(m5 + "/stage/x/y.bin");
    CHECK(reslot::analyze(m5).problem == reslot::Problem::NoSlots);
    CHECK(reslot::analyze(root + "/noexiste").problem == reslot::Problem::NotFound);

    // --- fallo a mitad: se deshace todo
    const std::string m6 = root + "/fallo";
    buildMario(m6, "c01", "01");
    const std::string snap6 = snapshot(m6);
    CHECK(reslot::makePlan(m6, {{1, 5}}, plan) == reslot::Problem::None);
    reslot::testFailAt = 9;
    CHECK(reslot::apply(plan, journal, nullptr, nullptr, err) == reslot::Problem::Io);
    reslot::testFailAt = -1;
    CHECK(snapshot(m6) == snap6);
    CHECK(!util::exists(journal));
    CHECK(!err.empty());

    // --- cancelación: nada cambia
    std::atomic<bool> cancel{true};
    CHECK(reslot::apply(plan, journal, nullptr, &cancel, err) == reslot::Problem::Cancelled);
    CHECK(snapshot(m6) == snap6 && !util::exists(journal));

    // --- apagón a mitad: recover() termina el trabajo
    reslot::testCrashAt = 15;
    CHECK(reslot::apply(plan, journal, nullptr, nullptr, err) == reslot::Problem::Io);
    reslot::testCrashAt = -1;
    CHECK(util::exists(journal));
    CHECK(snapshot(m6) != snap6);
    int resumed = reslot::recover(journal, err);
    CHECK(resumed > 0);
    CHECK(!util::exists(journal));
    const std::string m7 = root + "/referencia";  // lo mismo hecho sin interrupciones
    buildMario(m7, "c01", "01");
    CHECK(reslot::makePlan(m7, {{1, 5}}, plan) == reslot::Problem::None);
    CHECK(reslot::apply(plan, journal, nullptr, nullptr, err) == reslot::Problem::None);
    CHECK(snapshot(m6) == snapshot(m7));
    CHECK(reslot::recover(journal, err) == 0);  // sin diario no hay nada que hacer
    // --- apagón / fallo en CUALQUIER punto del plan: recover() siempre llega al resultado final y un fallo deja el original
    {
        const std::string ref = snapshot(m7);
        const std::string orig = snap6;
        bool allOk = true;
        for (int k = 0; k <= (int)plan.ops.size(); k++) {
            const std::string mk = root + "/barrido";
            util::rmrf(mk);
            buildMario(mk, "c01", "01");
            reslot::Plan pk;
            if (reslot::makePlan(mk, {{1, 5}}, pk) != reslot::Problem::None) allOk = false;
            reslot::testCrashAt = k;
            reslot::apply(pk, journal, nullptr, nullptr, err);
            reslot::testCrashAt = -1;
            if (reslot::recover(journal, err) < 0) allOk = false;
            std::string s1 = snapshot(mk);
            std::string r1 = ref;
            // los nombres de m7 y barrido solo difieren en la carpeta raíz: la instantánea es relativa, así que coinciden
            if (s1 != r1) allOk = false;
            util::rmrf(mk);
            buildMario(mk, "c01", "01");
            if (reslot::makePlan(mk, {{1, 5}}, pk) != reslot::Problem::None) allOk = false;
            reslot::testFailAt = k;
            reslot::apply(pk, journal, nullptr, nullptr, err);
            reslot::testFailAt = -1;
            if (snapshot(mk) != orig && k < (int)plan.ops.size()) allOk = false;
            util::rmrf(journal);
        }
        CHECK(allOk);
    }
    // diario corrupto
    util::writeFile(journal, "{{{");
    CHECK(reslot::recover(journal, err) == -1 && !err.empty());
    util::rmrf(journal + ".failed");
    util::rmrf(root);
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
    {  // MD5 incremental sobre la marcha (onData en orden) y reanudación desde un prefijo
        FakeTransport ft;
        ft.data = makeData(7u << 20);
        ft.delayMs = 5;
        http::SegOptions o;
        o.conns = 4;
        md5::Hasher h;
        o.onData = [&h](const char* p, size_t n) { h.update(p, n); };
        std::string got;
        http::SegResult r = runSeg(ft, o, got);
        CHECK(r.st == http::SegStatus::Done && got == ft.data);
        CHECK(r.written == ft.data.size());
        CHECK(h.finish() == md5::hex(ft.data));
        // reanudar: el archivo ya tiene los primeros 3 MB + 12345 bytes
        const size_t pre = (3u << 20) + 12345;
        FakeTransport ft2;
        ft2.data = ft.data;
        ft2.delayMs = 3;
        http::SegOptions o2;
        o2.conns = 3;
        o2.chunk = 512 * 1024;
        o2.start = pre;
        std::string seen;
        o2.onData = [&seen](const char* p, size_t n) { seen.append(p, n); };
        FILE* f = tmpfile();
        fwrite(ft.data.data(), 1, pre, f);
        http::Probe p = ft2.probe("u");
        Progress pr;
        http::SegResult r2 = http::segmentedDownload(ft2, "u", p, f, &pr, o2);
        fflush(f);
        std::string all((size_t)ftell(f), 0);
        rewind(f);
        CHECK(fread(&all[0], 1, all.size(), f) == all.size());
        fclose(f);
        CHECK(r2.st == http::SegStatus::Done && all == ft.data && seen == ft.data.substr(pre));
        CHECK(pr.now.load() == ft.data.size());
        // ya completo: no pide nada
        FakeTransport ft3;
        ft3.data = makeData(2u << 20);
        http::SegOptions o3;
        o3.start = ft3.data.size();
        std::string none;
        FILE* f3 = tmpfile();
        http::Probe p3 = ft3.probe("u");
        http::SegResult r3 = http::segmentedDownload(ft3, "u", p3, f3, nullptr, o3);
        fclose(f3);
        CHECK(r3.st == http::SegStatus::Done && ft3.calls.load() == 0 && r3.written == ft3.data.size());
    }
    {  // un trozo falla siempre: "written" indica el prefijo bueno, que sirve para reanudar
        FakeTransport ft;
        ft.data = makeData(8u << 20);
        ft.alwaysFailChunk = 3;
        http::SegOptions o;
        o.conns = 2;
        std::string got;
        http::SegResult r = runSeg(ft, o, got);
        CHECK(r.st == http::SegStatus::Failed);
        CHECK(r.written == got.size() && r.written == 3u * (1u << 20));
        CHECK(got == ft.data.substr(0, got.size()));
        // reanudar con la red ya arreglada
        ft.alwaysFailChunk = -1;
        http::SegOptions o2;
        o2.start = r.written;
        FILE* f = tmpfile();
        fwrite(got.data(), 1, got.size(), f);
        http::Probe p = ft.probe("u");
        http::SegResult r2 = http::segmentedDownload(ft, "u", p, f, nullptr, o2);
        fflush(f);
        std::string all((size_t)ftell(f), 0);
        rewind(f);
        CHECK(fread(&all[0], 1, all.size(), f) == all.size());
        fclose(f);
        CHECK(r2.st == http::SegStatus::Done && all == ft.data);
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
    testCategories();
    testReslot();
    testInstallerAndStore();
    testSegdl();
    printf(g_fail ? "\n%d FALLOS\n" : "\nTODO OK\n", g_fail);
    return g_fail ? 1 : 0;
}
