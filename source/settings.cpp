#include "settings.hpp"

#include "json.hpp"
#include "util.hpp"

Settings g_settings;

void Settings::load() {
    std::string text;
    if (!util::readFile(paths::settingsFile(), text)) return;
    json::Value v;
    if (!json::parse(text, v)) return;
    english = v["english"].boolean(english);
    gameId = (int)v["game_id"].i64(gameId);
    keepArchives = v["keep_archives"].boolean(keepArchives);
    verifyMd5 = v["verify_md5"].boolean(verifyMd5);
    perPage = (int)v["per_page"].i64(perPage);
    if (perPage < 8) perPage = 8;
    if (perPage > 50) perPage = 50;
    dlConns = (int)v["dl_conns"].i64(dlConns);
    if (dlConns < 1) dlConns = 1;
    if (dlConns > 6) dlConns = 6;
    if (gameId <= 0) gameId = 6498;
    systemKeyboard = v["system_keyboard"].boolean(systemKeyboard);
    musicMode = (int)v["music_mode"].i64(musicMode);
    if (musicMode < 0 || musicMode > 2) musicMode = 1;
    volume = (int)v["volume"].i64(volume);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
}

void Settings::save() const {
    json::Value v = json::Value::Obj();
    v.set("english", json::Value::B(english));
    v.set("game_id", json::Value::N(gameId));
    v.set("keep_archives", json::Value::B(keepArchives));
    v.set("verify_md5", json::Value::B(verifyMd5));
    v.set("per_page", json::Value::N(perPage));
    v.set("dl_conns", json::Value::N(dlConns));
    v.set("system_keyboard", json::Value::B(systemKeyboard));
    v.set("music_mode", json::Value::N(musicMode));
    v.set("volume", json::Value::N(volume));
    util::writeFileAtomic(paths::settingsFile(), v.dump(2));
}
