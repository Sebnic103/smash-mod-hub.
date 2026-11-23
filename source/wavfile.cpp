#include "wavfile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wav {

namespace {

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

struct File {
    FILE* f = nullptr;
    ~File() {
        if (f) fclose(f);
    }
};

int16_t clamp16(float v) {
    if (v > 32767.f) return 32767;
    if (v < -32768.f) return -32768;
    return (int16_t)v;
}

}  // namespace

bool load(const std::string& path, Clip& out, std::string& err, size_t maxBytes) {
    out = Clip();
    File file;
    file.f = fopen(path.c_str(), "rb");
    if (!file.f) {
        err = "no se pudo abrir";
        return false;
    }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, file.f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        err = "no es un WAV";
        return false;
    }
    // Busca los bloques "fmt " y "data" (puede haber otros en medio: LIST, fact...). Hasta 64 bloques.
    int format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool haveFmt = false;
    long dataPos = -1;
    uint64_t dataLen = 0;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, file.f) != 8) break;
        const uint32_t size = rd32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            if (size < 16 || size > 4096) {
                err = "bloque fmt raro";
                return false;
            }
            std::vector<uint8_t> f(size);
            if (fread(f.data(), 1, size, file.f) != size) {
                err = "archivo cortado";
                return false;
            }
            format = rd16(f.data());
            channels = rd16(f.data() + 2);
            rate = rd32(f.data() + 4);
            bits = rd16(f.data() + 14);
            if (format == 0xFFFE && size >= 26) format = rd16(f.data() + 24);  // extensible: el formato real va en el subtipo
            haveFmt = true;
            if (size & 1) fseek(file.f, 1, SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) {
            dataPos = ftell(file.f);
            dataLen = size;
            break;
        } else {
            if (fseek(file.f, (long)size + (long)(size & 1), SEEK_CUR) != 0) break;
        }
    }
    if (!haveFmt || dataPos < 0) {
        err = "faltan datos en el WAV";
        return false;
    }
    const bool isFloat = format == 3;
    if (!(format == 1 || isFloat)) {
        err = "formato no admitido (solo PCM sin comprimir)";
        return false;
    }
    if (isFloat ? bits != 32 : !(bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
        err = "profundidad de bits no admitida";
        return false;
    }
    if (channels < 1 || channels > 2) {
        err = "solo mono o estéreo";
        return false;
    }
    if (rate < 8000 || rate > 192000) {
        err = "frecuencia no admitida";
        return false;
    }
    // el bloque data puede declarar más de lo que hay (archivos cortados o con tamaño 0 de grabaciones en vivo)
    fseek(file.f, 0, SEEK_END);
    const long fileEnd = ftell(file.f);
    if (fileEnd < dataPos) {
        err = "archivo cortado";
        return false;
    }
    uint64_t avail = (uint64_t)(fileEnd - dataPos);
    if (dataLen == 0 || dataLen > avail) dataLen = avail;
    const uint64_t bytesPer = (uint64_t)(bits / 8) * (uint64_t)channels;
    uint64_t frames = dataLen / bytesPer;
    if (frames < 2) {
        err = "sin audio";
        return false;
    }
    if (frames * (uint64_t)channels * 2 > (uint64_t)maxBytes) {
        err = "demasiado largo para la memoria (maximo " + std::to_string(maxBytes / (1024 * 1024)) + " MB)";
        return false;
    }
    out.rate = (int)rate;
    out.channels = channels;
    try {
        out.pcm.resize((size_t)(frames * (uint64_t)channels));
    } catch (...) {
        out = Clip();
        err = "sin memoria";
        return false;
    }
    if (fseek(file.f, dataPos, SEEK_SET) != 0) {
        out = Clip();
        err = "error de lectura";
        return false;
    }
    // lectura por bloques de ~64 KB
    const size_t perBlock = std::max<size_t>(1, 65536 / (size_t)bytesPer);
    std::vector<uint8_t> buf(perBlock * (size_t)bytesPer);
    size_t done = 0;  // fotogramas
    while (done < (size_t)frames) {
        const size_t n = std::min(perBlock, (size_t)frames - done);
        if (fread(buf.data(), (size_t)bytesPer, n, file.f) != n) {
            out = Clip();
            err = "archivo cortado";
            return false;
        }
        const size_t samples = n * (size_t)channels;
        int16_t* dst = out.pcm.data() + done * (size_t)channels;
        const uint8_t* p = buf.data();
        for (size_t i = 0; i < samples; i++) {
            if (isFloat) {
                float v;
                memcpy(&v, p + i * 4, 4);
                if (!(v == v)) v = 0.f;  // NaN
                dst[i] = clamp16(v * 32767.f);
            } else if (bits == 16) {
                dst[i] = (int16_t)rd16(p + i * 2);
            } else if (bits == 8) {
                dst[i] = (int16_t)(((int)p[i] - 128) * 256);
            } else if (bits == 24) {
                dst[i] = (int16_t)rd16(p + i * 3 + 1);  // los 16 bits altos
            } else {
                dst[i] = (int16_t)rd16(p + i * 4 + 2);
            }
        }
        done += n;
    }
    return true;
}

void Player::render(float* out, int frames, int dstRate) {
    if (!clip_ || dstRate <= 0 || clip_->channels < 1 || clip_->pcm.empty()) {
        std::fill(out, out + (size_t)frames * 2, 0.f);
        return;
    }
    const Clip& c = *clip_;
    const size_t total = c.frames();
    const double step = (double)c.rate / (double)dstRate;
    const int16_t* s = c.pcm.data();
    const bool stereo = c.channels == 2;
    constexpr float k = 1.f / 32768.f;
    for (int i = 0; i < frames; i++) {
        if (pos_ >= (double)total - 1.0) {  // fin: silencio hasta que quien llama cambie de pista
            std::fill(out + (size_t)i * 2, out + (size_t)frames * 2, 0.f);
            pos_ = (double)total;  // marca como terminada (finished())
            return;
        }
        const size_t i0 = (size_t)pos_;
        const float f = (float)(pos_ - (double)i0);
        if (stereo) {
            const int16_t* a = s + i0 * 2;
            out[i * 2] = ((float)a[0] + ((float)a[2] - (float)a[0]) * f) * k;
            out[i * 2 + 1] = ((float)a[1] + ((float)a[3] - (float)a[1]) * f) * k;
        } else {
            const float v = ((float)s[i0] + ((float)s[i0 + 1] - (float)s[i0]) * f) * k;
            out[i * 2] = out[i * 2 + 1] = v;
        }
        pos_ += step;
    }
}

}  // namespace wav
