#include "synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace synth {

namespace {

// Canciones originales. Cada melodía respeta el acorde de su compás en los tiempos fuertes.
const SongDef kDefs[kSongs] = {
    {"Primer Round", 132, "C G Am F C G F G",
     "E5:2 G5:2 C6:4 B5:2 G5:2 E5:4 | D5:2 G5:2 B5:4 A5:2 G5:2 D5:4 | E5:2 A5:2 C6:4 B5:2 A5:2 E5:4 | F5:2 A5:2 C6:4 A5:2 G5:2 F5:4 | "
     "G5:4 E6:4 D6:2 C6:2 G5:4 | B5:4 D6:4 C6:2 B5:2 G5:4 | A5:4 C6:4 B5:2 A5:2 F5:4 | D6:2 B5:2 G5:2 B5:2 D6:8",
     0.5f, 0, 0, true},
    {"Arena Neon", 150, "Am F C G Am F G G",
     "A5:2 r:2 A5:2 C6:2 E6:4 C6:2 A5:2 | F5:2 r:2 F5:2 A5:2 C6:4 A5:2 F5:2 | G5:2 r:2 G5:2 C6:2 E6:4 C6:2 G5:2 | G5:2 r:2 B5:2 D6:2 G6:4 D6:2 B5:2 | "
     "E6:3 D6:3 C6:2 B5:2 A5:6 | C6:3 B5:3 A5:2 G5:2 F5:6 | D6:3 C6:3 B5:2 A5:2 G5:6 | B5:2 D6:2 G6:4 F6:2 D6:2 B5:4",
     0.25f, 0, 0, true},
    {"Descanso", 92, "F C Dm Bb F C Bb C",
     "A5:4 C6:4 A5:4 F5:4 | G5:4 E5:4 G5:4 C6:4 | A5:4 D6:4 A5:4 F5:4 | D6:4 F6:4 D6:4 Bb5:4 | "
     "C6:6 A5:2 F5:8 | E6:6 C6:2 G5:8 | D6:6 Bb5:2 F5:8 | G5:4 B5:4 C6:8",
     0.f, 1, 1, false},
};

constexpr double kTwoPi = 6.283185307179586;

}  // namespace

const SongDef& songDef(int i) { return kDefs[((i % kSongs) + kSongs) % kSongs]; }

double noteHz(const char* s, const char** end) {
    if (end) *end = s;
    if (!s) return 0.0;
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
    const char* p = s;
    if (*p < 'A' || *p > 'G') return 0.0;
    int semi = semis[*p - 'A'];
    p++;
    if (*p == '#') {
        semi++;
        p++;
    } else if (*p == 'b') {
        semi--;
        p++;
    }
    if (*p < '0' || *p > '9') return 0.0;
    const int oct = *p - '0';
    p++;
    if (end) *end = p;
    const int midi = 12 * (oct + 1) + semi;
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

bool parseChord(const std::string& s, int& rootSemi, bool& minor) {
    if (s.empty() || s[0] < 'A' || s[0] > 'G') return false;
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7};
    int semi = semis[s[0] - 'A'];
    size_t i = 1;
    if (i < s.size() && s[i] == '#') {
        semi++;
        i++;
    } else if (i < s.size() && s[i] == 'b') {
        semi--;
        i++;
    }
    minor = false;
    if (i < s.size() && s[i] == 'm') {
        minor = true;
        i++;
    }
    if (i != s.size()) return false;
    rootSemi = ((semi % 12) + 12) % 12;
    return true;
}

static std::vector<std::string> words(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool buildSteps(const SongDef& d, std::vector<Step>& out, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        out.clear();
        return false;
    };
    out.clear();
    const std::vector<std::string> chords = words(d.chords);
    if (chords.empty()) return fail("sin acordes");
    const int total = (int)chords.size() * 16;
    out.assign((size_t)total, Step());

    // melodía
    int pos = 0, barPos = 0;
    for (const std::string& tok : words(d.lead)) {
        if (tok == "|") {
            if (barPos != 0) return fail("compas incompleto en la melodia");
            continue;
        }
        const size_t colon = tok.find(':');
        if (colon == std::string::npos) return fail("falta ':' en " + tok);
        const int len = atoi(tok.c_str() + colon + 1);
        if (len <= 0 || len > 16) return fail("duracion invalida en " + tok);
        const std::string name = tok.substr(0, colon);
        float hz = 0.f;
        if (name != "r") {
            const char* e = nullptr;
            const double f = noteHz(name.c_str(), &e);
            if (f <= 0.0 || *e != '\0') return fail("nota invalida " + tok);
            hz = (float)f;
        }
        if (pos + len > total) return fail("la melodia es mas larga que los acordes");
        if (barPos + len > 16) return fail("una nota cruza el compas: " + tok);
        if (hz > 0.f) {
            out[(size_t)pos].leadHz = hz;
            out[(size_t)pos].leadLen = len;
        }
        pos += len;
        barPos = (barPos + len) % 16;
    }
    if (pos != total) return fail("la melodia no cubre todos los compases");

    // bajo, arpegio y batería a partir de los acordes
    for (size_t bar = 0; bar < chords.size(); bar++) {
        int root = 0;
        bool minor = false;
        if (!parseChord(chords[bar], root, minor)) return fail("acorde invalido " + chords[bar]);
        const int third = root + (minor ? 3 : 4), fifth = root + 7;
        auto hzOf = [](int semiAbove, int octave) { return (float)(440.0 * std::pow(2.0, ((12 * (octave + 1) + semiAbove) - 69) / 12.0)); };
        const int b0 = (int)bar * 16;
        // bajo (octava 2)
        if (d.bassStyle == 0) {
            for (int s = 0; s < 16; s += 2) {
                Step& st = out[(size_t)(b0 + s)];
                st.bassHz = hzOf(root, (s % 8 == 6) ? 3 : 2);
                st.bassLen = 2;
            }
        } else {
            out[(size_t)b0].bassHz = hzOf(root, 2);
            out[(size_t)b0].bassLen = 8;
            out[(size_t)(b0 + 8)].bassHz = hzOf(fifth, 2);
            out[(size_t)(b0 + 8)].bassLen = 8;
        }
        // arpegio (octava 4): raíz, tercera, quinta, octava, quinta, tercera
        const float tones[6] = {hzOf(root, 4), hzOf(third, 4), hzOf(fifth, 4), hzOf(root + 12, 4), hzOf(fifth, 4), hzOf(third, 4)};
        const int every = d.arpStyle == 0 ? 1 : 2;
        for (int s = 0, k = 0; s < 16; s += every, k++) out[(size_t)(b0 + s)].arpHz = tones[k % 6];
        // batería
        if (d.drums) {
            for (int s = 0; s < 16; s++) {
                uint8_t m = 0;
                if (s == 0 || s == 8) m |= kKick;
                if (s == 10 && d.bpm >= 140) m |= kKick;
                if (s == 4 || s == 12) m |= kSnare;
                if (s % 2 == 0) m |= kHat;
                out[(size_t)(b0 + s)].drums = m;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- render

void ChipSynth::init(int sampleRate) {
    rate_ = std::max(8000, std::min(sampleRate, 192000));
    setSong(0);
}

void ChipSynth::setSong(int idx) {
    song_ = ((idx % kSongs) + kSongs) % kSongs;
    const SongDef& d = songDef(song_);
    std::string err;
    if (!buildSteps(d, steps_, &err)) steps_.assign(16, Step());  // no debería pasar (lo comprueban los tests): silencio
    samplesPerStep_ = std::max(1, (int)((double)rate_ * 60.0 / (d.bpm * 4.0)));
    posInStep_ = 0;
    step_ = 0;
    loops_ = 0;
    lead_ = bass_ = arp_ = Voice();
    drum_[0] = drum_[1] = Drum();
    fade_ = 1.f;
    fadeTarget_ = 1.f;
    switching_ = false;
    startStep();
}

void ChipSynth::next() {
    switching_ = true;
    fadeTarget_ = 0.f;
}

const char* ChipSynth::name() const { return songDef(song_).name; }

float ChipSynth::noise() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (float)(int32_t)rng_ * (1.f / 2147483648.f);
}

void ChipSynth::trigger(Voice& v, float hz, int lenSamples) {
    v.inc = (double)hz / rate_;
    v.held = lenSamples;
    v.age = 0;
    v.on = true;
    // la fase no se reinicia: evita chasquidos al encadenar notas
}

void ChipSynth::startStep() {
    if (steps_.empty()) return;
    const Step& s = steps_[(size_t)step_];
    if (s.leadHz > 0.f) {
        trigger(lead_, s.leadHz, s.leadLen * samplesPerStep_ * 92 / 100);
        vibAge_ = 0;
    }
    if (s.bassHz > 0.f) trigger(bass_, s.bassHz, s.bassLen * samplesPerStep_ * 90 / 100);
    if (s.arpHz > 0.f) trigger(arp_, s.arpHz, samplesPerStep_ * (songDef(song_).arpStyle == 0 ? 80 : 160) / 100);
    for (int k : {kKick, kSnare, kHat}) {
        if (!(s.drums & k)) continue;
        Drum& d = drum_[k == kHat ? 1 : 0];  // bombo y caja comparten voz (no suenan a la vez); el charles aparte
        d.kind = k;
        d.age = 0;
        d.phase = 0.f;
    }
}

// onda * envolvente. wave: 0 pulso, 1 triángulo.
float ChipSynth::voiceSample(Voice& v, int wave, double duty, float sustain, float decayTau) {
    if (!v.on) return 0.f;
    // envolvente: ataque 3 ms, caída hacia "sustain", liberación 12 ms
    const float t = (float)v.age / rate_;
    float env;
    if (v.held > 0) {
        const float atk = std::min(1.f, t / 0.003f);
        env = (sustain + (1.f - sustain) * std::exp(-t / decayTau)) * atk;
        v.env = env;
        v.held--;
    } else {
        v.env -= 1.f / (0.012f * rate_) * std::max(v.env, 0.2f);
        if (v.env <= 0.f) {
            v.on = false;
            v.env = 0.f;
            return 0.f;
        }
        env = v.env;
    }
    v.age++;
    v.phase += v.inc;
    if (v.phase >= 1.0) v.phase -= 1.0;
    float w;
    if (wave == 0) {
        // pulso sin componente continua (un pulso estrecho tiene "media" distinta de 0 y, al cambiar la envolvente,
        // provoca chasquidos): se resta la media y se normaliza el pico a 1
        const float hi = (float)(1.0 - duty), lo = (float)duty;
        w = (v.phase < duty ? hi : -lo) / std::max(hi, lo);
    } else w = (float)(4.0 * std::fabs(v.phase - 0.5) - 1.0);
    return w * env;
}

void ChipSynth::render(float* out, int frames) {
    const SongDef& def = songDef(song_);
    const int leadWave = def.duty > 0.f ? 0 : 1;
    const float fadeStep = 1.f / (0.35f * rate_);
    const float lpA = 1.f - std::exp(-(float)(kTwoPi * 9000.0 / rate_));  // filtro suave: quita el brillo áspero del pulso
    for (int i = 0; i < frames; i++) {
        if (posInStep_ >= samplesPerStep_) {
            posInStep_ = 0;
            step_++;
            if ((size_t)step_ >= steps_.size()) {
                step_ = 0;
                if (++loops_ >= 2) {  // cada canción suena dos vueltas y pasa a la siguiente
                    loops_ = 0;
                    switching_ = true;
                    fadeTarget_ = 0.f;
                }
            }
            startStep();
        }
        posInStep_++;

        // fundido de cambio de canción
        if (fade_ < fadeTarget_) fade_ = std::min(fadeTarget_, fade_ + fadeStep);
        else if (fade_ > fadeTarget_) fade_ = std::max(fadeTarget_, fade_ - fadeStep);
        if (switching_ && fade_ <= 0.f) {
            const int nx = (song_ + 1) % kSongs;
            setSong(nx);
            fade_ = 0.f;
            fadeTarget_ = 1.f;
            return render(out + (size_t)i * 2, frames - i);  // el resto del búfer ya con la nueva canción
        }

        // melodía con un poco de vibrato (solo si la onda es triangular/suave) tras 120 ms
        float l = 0.f;
        if (lead_.on) {
            if (def.duty == 0.f && vibAge_ > rate_ / 8) {
                const double vib = 1.0 + 0.0035 * std::sin(kTwoPi * 5.5 * (double)vibAge_ / rate_);
                lead_.phase += lead_.inc * (vib - 1.0);
            }
            vibAge_++;
            l = voiceSample(lead_, leadWave, def.duty, 0.7f, 0.18f) * (def.duty == 0.f ? 0.30f : 0.20f);
        }
        const float b = voiceSample(bass_, 1, 0.5, 0.85f, 0.4f) * 0.30f;
        const float a = voiceSample(arp_, 0, def.arpStyle == 0 ? 0.125 : 0.25, 0.4f, 0.08f) * (def.arpStyle == 0 ? 0.075f : 0.06f);

        // batería: bombo/caja en drum_[0], charles en drum_[1]
        float dr = 0.f;
        for (int k = 0; k < 2; k++) {
            Drum& d = drum_[k];
            if (d.age < 0) continue;
            const float t = (float)d.age / rate_;
            if (d.kind == kKick) {
                const float f = 45.f + 90.f * std::exp(-t / 0.03f);
                d.phase += f / rate_;
                if (d.phase >= 1.f) d.phase -= 1.f;
                dr += std::sin((float)kTwoPi * d.phase) * std::exp(-t / 0.09f) * 0.42f;
                if (t > 0.3f) d.age = -2;
            } else if (d.kind == kSnare) {
                d.phase += 190.f / rate_;
                if (d.phase >= 1.f) d.phase -= 1.f;
                dr += (noise() * 0.5f + std::sin((float)kTwoPi * d.phase) * 0.35f) * std::exp(-t / 0.07f) * 0.30f;
                if (t > 0.3f) d.age = -2;
            } else {  // charles: ruido con pasa-altos
                const float n = noise();
                d.lp += (n - d.lp) * 0.3f;
                dr += (n - d.lp) * std::exp(-t / 0.018f) * 0.13f;
                if (t > 0.1f) d.age = -2;
            }
            d.age = d.age == -2 ? -1 : d.age + 1;
        }

        const float mono = (l + b + a + dr) * fade_;
        // pequeña apertura estéreo: melodía a la derecha, arpegio a la izquierda
        float L = mono - l * 0.25f * fade_ + a * 0.5f * fade_;
        float R = mono + l * 0.25f * fade_ - a * 0.5f * fade_;
        lp_[0] += (L - lp_[0]) * lpA;
        lp_[1] += (R - lp_[1]) * lpA;
        out[i * 2] = lp_[0];
        out[i * 2 + 1] = lp_[1];
    }
}

}  // namespace synth
