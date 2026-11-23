// Pruebas de la música: notas y acordes, canciones, sintetizador, lectura de .wav, reproductor y motor.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../source/music.hpp"
#include "../source/synth.hpp"
#include "../source/wavfile.hpp"

static int g_fail = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("  FALLO %s:%d  %s\n", __FILE__, __LINE__, #c);  \
            g_fail++;                                               \
        }                                                           \
    } while (0)

static std::string tmpPath(const char* name) {
    const char* d = getenv("TMPDIR");
    return std::string(d ? d : "/tmp") + "/smh_audio_" + name;
}

// ---------------------------------------------------------------- notas, acordes y canciones

static void testNotes() {
    CHECK(std::fabs(synth::noteHz("A4") - 440.0) < 0.001);
    CHECK(std::fabs(synth::noteHz("C4") - 261.6256) < 0.01);
    CHECK(std::fabs(synth::noteHz("A5") - 880.0) < 0.001);
    CHECK(std::fabs(synth::noteHz("Bb5") - 932.3275) < 0.01);
    CHECK(std::fabs(synth::noteHz("F#4") - 369.9944) < 0.01);
    CHECK(synth::noteHz("H4") == 0.0);
    CHECK(synth::noteHz("A") == 0.0);
    CHECK(synth::noteHz("") == 0.0);
    CHECK(synth::noteHz(nullptr) == 0.0);
    const char* e = nullptr;
    synth::noteHz("E5:2", &e);
    CHECK(e && *e == ':');

    int r = 0;
    bool m = false;
    CHECK(synth::parseChord("Am", r, m) && r == 9 && m);
    CHECK(synth::parseChord("C", r, m) && r == 0 && !m);
    CHECK(synth::parseChord("Bb", r, m) && r == 10 && !m);
    CHECK(synth::parseChord("F#m", r, m) && r == 6 && m);
    CHECK(!synth::parseChord("", r, m));
    CHECK(!synth::parseChord("H", r, m));
    CHECK(!synth::parseChord("Cmaj7", r, m));
}

static void testSongs() {
    for (int i = 0; i < synth::kSongs; i++) {
        const synth::SongDef& d = synth::songDef(i);
        std::vector<synth::Step> st;
        std::string err;
        const bool ok = synth::buildSteps(d, st, &err);
        if (!ok) printf("  canción %d (%s): %s\n", i, d.name, err.c_str());
        CHECK(ok);
        CHECK(st.size() == 128);
        // la melodía llena todos los compases: cada nota empieza donde acaba la anterior (o hay silencio explícito)
        int notes = 0, lenSum = 0;
        for (const auto& s : st)
            if (s.leadHz > 0.f) {
                notes++;
                lenSum += s.leadLen;
                CHECK(s.leadHz > 200.f && s.leadHz < 2000.f);
            }
        CHECK(notes >= 24);
        CHECK(lenSum <= 128);
        // el bajo suena en el primer paso de cada compás
        for (int bar = 0; bar < 8; bar++) {
            CHECK(st[(size_t)bar * 16].bassHz > 40.f && st[(size_t)bar * 16].bassHz < 400.f);
            CHECK(st[(size_t)bar * 16].arpHz > 200.f);
        }
        int drums = 0;
        for (const auto& s : st) drums += s.drums ? 1 : 0;
        CHECK((drums > 0) == d.drums);
    }
    // el primer acorde de "Primer Round" es Do: bajo C2 (65.4 Hz)
    std::vector<synth::Step> st;
    synth::buildSteps(synth::songDef(0), st);
    CHECK(std::fabs(st[0].bassHz - 65.406f) < 0.1f);
    CHECK(std::fabs(st[0].leadHz - (float)synth::noteHz("E5")) < 0.01f);
    // Descanso: el bajo es una redonda partida en dos mitades
    synth::buildSteps(synth::songDef(2), st);
    CHECK(st[0].bassLen == 8 && st[8].bassLen == 8 && st[2].bassHz == 0.f);
}

static void testBadNotation() {
    std::vector<synth::Step> st;
    std::string err;
    synth::SongDef d = {"x", 120, "C G", "C5:16 | G5:8", 0.5f, 0, 0, false};
    CHECK(!synth::buildSteps(d, st, &err) && !err.empty());  // falta medio compás
    d.lead = "C5:16 | G5:20";
    CHECK(!synth::buildSteps(d, st, &err));  // duración imposible
    d.lead = "C5:10 G5:10 | x";
    CHECK(!synth::buildSteps(d, st, &err));  // nota cruza el compás
    d.lead = "C5:16 | H5:16";
    CHECK(!synth::buildSteps(d, st, &err));  // nota inválida
    d.lead = "C5:16 | G5";
    CHECK(!synth::buildSteps(d, st, &err));  // sin duración
    d.lead = "C5:16 | G5:16";
    d.chords = "C Zm";
    CHECK(!synth::buildSteps(d, st, &err));  // acorde inválido
    d.chords = "C G";
    CHECK(synth::buildSteps(d, st, &err));
    CHECK(st.size() == 32);
    d.lead = "C5:16 | G5:16 | E5:16";
    CHECK(!synth::buildSteps(d, st, &err));  // melodía más larga que los acordes
}

// ---------------------------------------------------------------- sintetizador

static void stats(const std::vector<float>& v, double& peak, double& rms, double& mean, bool& finite) {
    peak = 0;
    double acc = 0, sum = 0;
    finite = true;
    for (float x : v) {
        if (!std::isfinite(x)) finite = false;
        peak = std::max(peak, (double)std::fabs(x));
        acc += (double)x * x;
        sum += x;
    }
    rms = std::sqrt(acc / v.size());
    mean = sum / v.size();
}

static void testSynthRender() {
    for (int song = 0; song < synth::kSongs; song++) {
        synth::ChipSynth s;
        s.init(48000);
        s.setSong(song);
        std::vector<float> out(48000 * 2 * 12);
        for (size_t off = 0; off < out.size(); off += 2048 * 2) s.render(out.data() + off, (int)std::min<size_t>(2048, (out.size() - off) / 2));
        double peak, rms, mean;
        bool fin;
        stats(out, peak, rms, mean, fin);
        printf("  canción %d: pico %.3f  rms %.3f  media %.5f\n", song, peak, rms, mean);
        CHECK(fin);
        CHECK(peak < 1.0);   // sin recortes antes del volumen general
        CHECK(peak > 0.15);  // y no está casi muda
        CHECK(rms > 0.03 && rms < 0.4);
        CHECK(std::fabs(mean) < 0.01);  // sin componente continua
    }
    // determinista: dos instancias dan exactamente lo mismo, también con búferes de distinto tamaño
    synth::ChipSynth a, b;
    a.init(48000);
    b.init(48000);
    std::vector<float> oa(48000 * 2 * 3), ob(48000 * 2 * 3);
    a.render(oa.data(), 48000 * 3);
    for (int off = 0; off < 48000 * 3; off += 777) b.render(ob.data() + (size_t)off * 2, std::min(777, 48000 * 3 - off));
    CHECK(oa == ob);
    // otras frecuencias de muestreo
    for (int rate : {22050, 44100, 96000}) {
        synth::ChipSynth s;
        s.init(rate);
        std::vector<float> out((size_t)rate * 2 * 2);
        s.render(out.data(), rate * 2);
        double peak, rms, mean;
        bool fin;
        stats(out, peak, rms, mean, fin);
        CHECK(fin && peak < 1.0 && rms > 0.03);
    }
    // frames = 0 no hace nada
    synth::ChipSynth z;
    z.init(48000);
    z.render(nullptr, 0);
}

static void testSongSwitching() {
    synth::ChipSynth s;
    s.init(48000);
    CHECK(s.song() == 0);
    std::vector<float> buf(4096 * 2);
    // next() pasa a la siguiente tras un fundido corto
    s.next();
    for (int i = 0; i < 20; i++) s.render(buf.data(), 4096);  // ~1,7 s
    CHECK(s.song() == 1);
    // sin pulsar nada, cada canción suena dos vueltas y cambia sola: 14,5 s x 2 + fundido para la canción 1 (150 bpm: 12,8 s x 2)
    int guard = 0;
    while (s.song() == 1 && guard++ < 2000) s.render(buf.data(), 4096);
    CHECK(s.song() == 2);
    double peak = 0;
    for (int i = 0; i < 40; i++) {
        s.render(buf.data(), 4096);
        for (float x : buf) peak = std::max(peak, (double)std::fabs(x));
    }
    CHECK(peak > 0.1 && peak < 1.0);
    // después de la última vuelve a la primera
    guard = 0;
    while (s.song() == 2 && guard++ < 4000) s.render(buf.data(), 4096);
    CHECK(s.song() == 0);
    s.setSong(-1);  // fuera de rango: se ajusta
    CHECK(s.song() == synth::kSongs - 1);
    CHECK(s.name() != nullptr && std::strlen(s.name()) > 3);
}

// ---------------------------------------------------------------- .wav

static void put16(std::string& s, unsigned v) { s += (char)(v & 255); s += (char)((v >> 8) & 255); }
static void put32(std::string& s, unsigned v) { put16(s, v & 0xFFFF); put16(s, v >> 16); }

// Construye un WAV. frames de una onda cuadrada simple; extra = bloque adicional antes de "data" (p. ej. LIST).
static std::string makeWav(int fmt, int channels, int rate, int bits, int frames, bool extensible = false, const std::string& extra = "", bool zeroSize = false) {
    std::string data;
    const int bytes = bits / 8;
    for (int i = 0; i < frames; i++) {
        const double v = (i / 4) % 2 ? 0.5 : -0.5;
        for (int c = 0; c < channels; c++) {
            const double vv = c == 0 ? v : v / 2;
            if (fmt == 3) {
                float f = (float)vv;
                unsigned u;
                memcpy(&u, &f, 4);
                put32(data, u);
            } else if (bits == 8) {
                data += (char)(int)(128 + vv * 127);
            } else if (bits == 16) {
                put16(data, (unsigned)(int)(vv * 32767) & 0xFFFF);
            } else if (bits == 24) {
                int x = (int)(vv * 8388607);
                data += (char)(x & 255); data += (char)((x >> 8) & 255); data += (char)((x >> 16) & 255);
            } else {
                int x = (int)(vv * 2147483647.0);
                put32(data, (unsigned)x);
            }
        }
        (void)bytes;
    }
    std::string fmtChunk;
    put16(fmtChunk, extensible ? 0xFFFE : (unsigned)fmt);
    put16(fmtChunk, (unsigned)channels);
    put32(fmtChunk, (unsigned)rate);
    put32(fmtChunk, (unsigned)(rate * channels * bits / 8));
    put16(fmtChunk, (unsigned)(channels * bits / 8));
    put16(fmtChunk, (unsigned)bits);
    if (extensible) {
        put16(fmtChunk, 22);
        put16(fmtChunk, (unsigned)bits);
        put32(fmtChunk, 3);
        put16(fmtChunk, (unsigned)fmt);  // subtipo: los 2 primeros bytes del GUID
        fmtChunk += std::string(14, '\0');
    }
    std::string body = "WAVE";
    body += "fmt ";
    put32(body, (unsigned)fmtChunk.size());
    body += fmtChunk;
    body += extra;
    body += "data";
    put32(body, zeroSize ? 0u : (unsigned)data.size());
    body += data;
    std::string all = "RIFF";
    put32(all, (unsigned)body.size());
    all += body;
    return all;
}

static bool writeFile(const std::string& path, const std::string& bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    return true;
}

static void testWavLoad() {
    const std::string p = tmpPath("a.wav");
    wav::Clip c;
    std::string err;
    // 16 bits estéreo
    CHECK(writeFile(p, makeWav(1, 2, 44100, 16, 1000)));
    CHECK(wav::load(p, c, err, 1 << 20));
    CHECK(c.rate == 44100 && c.channels == 2 && c.frames() == 1000);
    CHECK(c.pcm[0] < -15000 && c.pcm[8] > 15000);  // la onda cuadrada se ve
    // 16 bits mono
    writeFile(p, makeWav(1, 1, 22050, 16, 500));
    CHECK(wav::load(p, c, err, 1 << 20) && c.channels == 1 && c.frames() == 500);
    // 8, 24 y 32 bits enteros y float
    for (int bits : {8, 24, 32}) {
        writeFile(p, makeWav(1, 2, 48000, bits, 300));
        CHECK(wav::load(p, c, err, 1 << 20));
        CHECK(c.frames() == 300);
        CHECK(c.pcm[0] < -10000 && c.pcm[8] > 10000);
    }
    writeFile(p, makeWav(3, 2, 48000, 32, 300));
    CHECK(wav::load(p, c, err, 1 << 20));
    CHECK(c.pcm[0] < -10000 && c.pcm[8] > 10000);
    // "extensible" (lo que escriben muchos editores con 24 bits o más de 2 canales)
    writeFile(p, makeWav(1, 2, 48000, 24, 300, true));
    CHECK(wav::load(p, c, err, 1 << 20) && c.frames() == 300);
    // bloques extra antes de los datos (LIST con tamaño impar -> relleno)
    std::string list = "LIST";
    put32(list, 5);
    list += std::string(5, 'x') + std::string(1, '\0');
    writeFile(p, makeWav(1, 2, 44100, 16, 200, false, list));
    CHECK(wav::load(p, c, err, 1 << 20) && c.frames() == 200);
    // tamaño de datos 0 (grabación en curso): se usa lo que haya
    writeFile(p, makeWav(1, 2, 44100, 16, 200, false, "", true));
    CHECK(wav::load(p, c, err, 1 << 20) && c.frames() == 200);
    // errores: demasiado largo, cortado, formato, canales, frecuencia, basura, inexistente
    writeFile(p, makeWav(1, 2, 44100, 16, 100000));
    CHECK(!wav::load(p, c, err, 100000) && err.find("memoria") != std::string::npos);
    CHECK(c.pcm.empty());
    std::string w = makeWav(1, 2, 44100, 16, 1000);
    writeFile(p, w.substr(0, 30));
    CHECK(!wav::load(p, c, err, 1 << 20));
    writeFile(p, makeWav(2, 2, 44100, 16, 100));  // ADPCM
    CHECK(!wav::load(p, c, err, 1 << 20) && !err.empty());
    writeFile(p, makeWav(1, 3, 44100, 16, 100));
    CHECK(!wav::load(p, c, err, 1 << 20));
    writeFile(p, makeWav(1, 2, 1000, 16, 100));
    CHECK(!wav::load(p, c, err, 1 << 20));
    writeFile(p, "esto no es un wav, es texto");
    CHECK(!wav::load(p, c, err, 1 << 20));
    writeFile(p, makeWav(1, 2, 44100, 16, 1));
    CHECK(!wav::load(p, c, err, 1 << 20));  // un solo fotograma
    CHECK(!wav::load(tmpPath("no_existe.wav"), c, err, 1 << 20));
    // cortado a mitad de los datos pero con cabecera que promete más: se acepta lo que hay
    w = makeWav(1, 2, 44100, 16, 1000);
    writeFile(p, w.substr(0, w.size() - 400));
    CHECK(wav::load(p, c, err, 1 << 20) && c.frames() == 900);
    remove(p.c_str());
}

static void testPlayer() {
    auto clip = std::make_shared<wav::Clip>();
    clip->rate = 24000;
    clip->channels = 1;
    for (int i = 0; i < 2400; i++) clip->pcm.push_back((int16_t)(i % 2 ? 16384 : -16384));
    wav::Player p;
    std::vector<float> out(2 * 1000);
    p.render(out.data(), 1000, 48000);  // sin pista: silencio
    for (float x : out) CHECK(x == 0.f);
    p.set(clip);
    CHECK(p.hasClip() && !p.finished());
    p.render(out.data(), 1000, 48000);
    // 24 kHz -> 48 kHz: el doble de fotogramas; los canales de un mono son iguales
    for (int i = 0; i < 1000; i++) CHECK(out[(size_t)i * 2] == out[(size_t)i * 2 + 1]);
    double peak = 0;
    for (float x : out) peak = std::max(peak, (double)std::fabs(x));
    CHECK(peak > 0.1 && peak <= 0.51);
    // 2400 fotogramas a 24 kHz = 4800 a 48 kHz: tras 5000 ya terminó y rellena con silencio
    int guard = 0;
    while (!p.finished() && guard++ < 100) p.render(out.data(), 1000, 48000);
    CHECK(p.finished());
    CHECK(guard >= 4 && guard <= 6);
    p.render(out.data(), 1000, 48000);
    for (float x : out) CHECK(x == 0.f);
    p.set(nullptr);
    CHECK(!p.hasClip());
    // pista vacía o corrupta no rompe nada
    auto bad = std::make_shared<wav::Clip>();
    bad->rate = 44100;
    p.set(bad);
    p.render(out.data(), 1000, 48000);
    CHECK(true);
}

// ---------------------------------------------------------------- motor

static double rmsOf(const std::vector<int16_t>& v) {
    double a = 0;
    for (int16_t x : v) a += (double)x * x;
    return std::sqrt(a / std::max<size_t>(1, v.size())) / 32768.0;
}

static void testEngine() {
    music::Engine e;
    e.init(48000);
    e.setVolume(60);
    std::vector<int16_t> buf(2048 * 2);
    // apagado: silencio total y en reposo
    CHECK(e.idle());
    e.render(buf.data(), 2048);
    CHECK(rmsOf(buf) == 0.0);
    CHECK(e.level() == 0.f);
    // chiptune: suena con fundido de entrada
    e.setMode(music::Mode::Chip);
    CHECK(!e.idle());
    e.render(buf.data(), 2048);
    const double first = rmsOf(buf);
    for (int i = 0; i < 50; i++) e.render(buf.data(), 2048);
    const double later = rmsOf(buf);
    printf("  motor: rms inicial %.4f, rms después %.4f, nivel %.2f\n", first, later, e.level());
    CHECK(first < later);  // entra con fundido
    CHECK(later > 0.01 && later < 0.6);
    CHECK(e.level() > 0.05f && e.level() <= 1.f);
    // sin recortes duros
    int clipped = 0;
    for (int i = 0; i < 100; i++) {
        e.render(buf.data(), 2048);
        for (int16_t x : buf) clipped += (x >= 32767 || x <= -32768) ? 1 : 0;
    }
    CHECK(clipped == 0);
    // volumen 0 = silencio; volumen alto = más fuerte; sin saltos bruscos al cambiarlo
    e.setVolume(100);
    for (int i = 0; i < 30; i++) e.render(buf.data(), 2048);
    const double loud = rmsOf(buf);
    CHECK(loud > later);
    e.setVolume(0);
    for (int i = 0; i < 30; i++) e.render(buf.data(), 2048);
    CHECK(rmsOf(buf) < 0.0005);
    CHECK(music::masterGain(0) == 0.f);
    CHECK(music::masterGain(100) <= 0.81f && music::masterGain(100) > 0.7f);
    CHECK(music::masterGain(30) < music::masterGain(60));
    CHECK(music::masterGain(-5) == 0.f && music::masterGain(500) == music::masterGain(100));
    // apagar: el fundido termina y vuelve a estar en reposo
    e.setVolume(60);
    for (int i = 0; i < 20; i++) e.render(buf.data(), 2048);
    e.setMode(music::Mode::Off);
    for (int i = 0; i < 10; i++) e.render(buf.data(), 2048);  // ~0,4 s
    CHECK(e.idle());
    e.render(buf.data(), 2048);
    CHECK(rmsOf(buf) == 0.0);
    // música personalizada
    auto clip = std::make_shared<wav::Clip>();
    clip->rate = 48000;
    clip->channels = 2;
    for (int i = 0; i < 48000 * 2; i++) {
        clip->pcm.push_back((int16_t)(12000 * std::sin(i * 0.05)));
        clip->pcm.push_back((int16_t)(12000 * std::sin(i * 0.05)));
    }
    e.setClip(clip);
    e.setMode(music::Mode::Custom);
    for (int i = 0; i < 10; i++) e.render(buf.data(), 2048);
    CHECK(rmsOf(buf) > 0.01);
    CHECK(e.hasClip() && !e.clipFinished());
    int guard = 0;
    while (!e.clipFinished() && guard++ < 200) e.render(buf.data(), 2048);
    CHECK(e.clipFinished());
    // tamaños de búfer raros
    for (int frames : {1, 3, 7, 100, 4096}) {
        std::vector<int16_t> b((size_t)frames * 2);
        e.setMode(music::Mode::Chip);
        e.render(b.data(), frames);
    }
    e.render(nullptr, 0);
}

int main() {
    testNotes();
    testSongs();
    testBadNotation();
    testSynthRender();
    testSongSwitching();
    testWavLoad();
    testPlayer();
    testEngine();
    if (g_fail) {
        printf("test_audio: %d FALLOS\n", g_fail);
        return 1;
    }
    printf("test_audio: todo OK\n");
    return 0;
}
