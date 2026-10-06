#include "world/Noise.h"
#include <cmath>

namespace df {
Perlin2D::Perlin2D(uint64_t seed) {
    Rng r(seed);
    for (int i = 0; i < 256; ++i) perm_[i] = (uint8_t)i;
    for (int i = 255; i > 0; --i) {
        int j = (int)(r.next() % (uint64_t)(i + 1));
        uint8_t t = perm_[i]; perm_[i] = perm_[j]; perm_[j] = t;
    }
    for (int i = 0; i < 256; ++i) perm_[256 + i] = perm_[i];
    for (int i = 0; i < 256; ++i) {
        float a = r.uniform(0.0f, 6.2831853f);
        gx_[i] = std::cos(a);
        gy_[i] = std::sin(a);
    }
    ox_ = r.uniform(-1000.0f, 1000.0f);
    oy_ = r.uniform(-1000.0f, 1000.0f);
}

float Perlin2D::operator()(float x, float y) const {
    x += ox_;
    y += oy_;
    float fx = std::floor(x), fy = std::floor(y);
    int xi = (int)fx & 255, yi = (int)fy & 255;
    float xf = x - fx, yf = y - fy;
    auto grad = [&](int ix, int iy, float dx, float dy) {
        int h = perm_[perm_[ix & 255] + (iy & 255)];
        return gx_[h] * dx + gy_[h] * dy;
    };
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    float n00 = grad(xi, yi, xf, yf), n10 = grad(xi + 1, yi, xf - 1, yf);
    float n01 = grad(xi, yi + 1, xf, yf - 1), n11 = grad(xi + 1, yi + 1, xf - 1, yf - 1);
    float u = fade(xf), v = fade(yf);
    float a = n00 + u * (n10 - n00), b = n01 + u * (n11 - n01);
    return (a + v * (b - a)) * 1.41421356f;
}

float fbm(const Perlin2D& n, float x, float y, int octaves, float lac, float gain) {
    float total = 0, amp = 1, freq = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        float a = 0.5f * i, ca = std::cos(a), sa = std::sin(a);   // rotate octaves against grid artefacts
        total += amp * n((x * ca - y * sa) * freq, (x * sa + y * ca) * freq);
        norm += amp;
        amp *= gain;
        freq *= lac;
    }
    return total / norm;
}

float ridged(const Perlin2D& n, float x, float y, int octaves, float lac, float gain, float sharp) {
    float total = 0, weight = 1, amp = 1, freq = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        float a = 0.37f * i, ca = std::cos(a), sa = std::sin(a);
        float v = n((x * ca - y * sa) * freq, (x * sa + y * ca) * freq);
        v = std::pow(1.0f - std::abs(v), sharp) * weight;
        weight = std::fmin(std::fmax(v * 1.6f, 0.0f), 1.0f);
        total += v * amp;
        norm += amp;
        amp *= gain;
        freq *= lac;
    }
    return total / norm;
}

float smoothstep(float e0, float e1, float x) {
    if (e1 == e0) return x >= e0 ? 1.0f : 0.0f;
    float t = std::fmin(std::fmax((x - e0) / (e1 - e0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}
float hash01(uint32_t a, uint32_t b, uint32_t c) {
    return (hash32(a ^ hash32(b ^ hash32(c + 0x9e3779b9))) >> 8) * (1.0f / 16777216.0f);
}
}  // namespace df
