#include "md5.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace md5 {
namespace {

struct Ctx {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    size_t fill = 0;
    uint32_t K[64];
    Ctx() {
        for (int i = 0; i < 64; i++) K[i] = (uint32_t)(std::fabs(std::sin((double)(i + 1))) * 4294967296.0);
    }

    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void block(const uint8_t* p) {
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t M[16];
        for (int i = 0; i < 16; i++) M[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; i++) {
            uint32_t F;
            int g;
            if (i < 16) {
                F = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | (~D & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                F = C ^ (B | ~D);
                g = (7 * i) % 16;
            }
            F = F + A + K[i] + M[g];
            A = D;
            D = C;
            C = B;
            B = B + rol(F, S[i]);
        }
        a += A;
        b += B;
        c += C;
        d += D;
    }

    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n > 0) {
            size_t take = 64 - fill;
            if (take > n) take = n;
            memcpy(buf + fill, p, take);
            fill += take;
            p += take;
            n -= take;
            if (fill == 64) {
                block(buf);
                fill = 0;
            }
        }
    }

    std::string finish() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (fill != 56) update(&zero, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (8 * i));
        update(lb, 8);
        char out[33];
        uint32_t r[4] = {a, b, c, d};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) snprintf(out + (i * 4 + j) * 2, 3, "%02x", (r[i] >> (8 * j)) & 0xFF);
        return std::string(out, 32);
    }
};

}  // namespace

struct Hasher::Impl {
    Ctx c;
};
Hasher::Hasher() : p_(new Impl) {}
Hasher::~Hasher() { delete p_; }
void Hasher::update(const void* data, size_t n) { p_->c.update((const uint8_t*)data, n); }
std::string Hasher::finish() { return p_->c.finish(); }

std::string hex(const std::string& data) {
    Ctx c;
    c.update((const uint8_t*)data.data(), data.size());
    return c.finish();
}

std::string file(const std::string& path, Progress* progress) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    Ctx c;
    std::vector<uint8_t> buf(1 << 16);
    size_t n;
    uint64_t done = 0;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
        if (progress && progress->cancel.load()) {
            fclose(f);
            return "";
        }
        c.update(buf.data(), n);
        done += n;
        if (progress) progress->now = done;
    }
    fclose(f);
    return c.finish();
}

}  // namespace md5
