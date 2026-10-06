#include "render/SkyModel.h"
#include <glm/gtc/packing.hpp>
#include <cmath>

namespace df {
namespace {
constexpr float kRe = 6360e3f, kRa = 6420e3f, kHr = 8000.0f, kHm = 1200.0f;
// Calibrated against the reference-world Blender renders: a strength-1 sky
// texture (zenith ~2 blue, horizon ~6-8 at low sun); the map's sky strength scales it.
constexpr float kSunRadiance = 80.0f;   // x4 (Oct 2026): shadows were nearly black next to the Blender renders

float raySphere(vec3 o, vec3 d, float r) {   // far intersection, -1 if none
    float b = glm::dot(o, d), c = glm::dot(o, o) - r * r, disc = b * b - c;
    if (disc < 0) return -1.0f;
    return -b + std::sqrt(disc);
}
bool hitsGround(vec3 o, vec3 d) {
    float b = glm::dot(o, d), c = glm::dot(o, o) - kRe * kRe, disc = b * b - c;
    return disc >= 0 && (-b - std::sqrt(disc)) > 0;
}

struct Atmos {
    vec3 betaR, betaM, betaO;
    float mieExt;
    vec3 extinction(float h, vec3& rR, vec3& rM) const {
        float dr = std::exp(-h / kHr), dm = std::exp(-h / kHm);
        float doz = std::max(0.0f, 1.0f - std::abs(h - 25000.0f) / 15000.0f);
        rR = betaR * dr;
        rM = betaM * dm;
        return betaR * dr + betaM * mieExt * dm + betaO * doz;
    }
};

vec3 transmittanceToSun(const Atmos& a, vec3 p, vec3 s) {
    if (hitsGround(p, s)) return vec3(0.0f);
    float t = raySphere(p, s, kRa);
    const int n = 8;
    float ds = t / n;
    vec3 od(0.0f), rR, rM;
    for (int i = 0; i < n; ++i) {
        vec3 q = p + s * ((i + 0.5f) * ds);
        od += a.extinction(glm::length(q) - kRe, rR, rM) * ds;
    }
    return glm::exp(-od);
}

vec3 skyRadiance(const Atmos& a, vec3 dir, vec3 sun) {
    vec3 o(0, 0, kRe + 100.0f);
    if (dir.z < -0.02f) {   // below the horizon: use the horizon at the same azimuth (a ray into the planet would overflow)
        vec2 h(dir.x, dir.y);
        h = glm::length(h) > 1e-6f ? glm::normalize(h) : vec2(1, 0);
        dir = glm::normalize(vec3(h * 0.9998f, -0.02f));
    }
    float tMax = raySphere(o, dir, kRa);
    const int n = 24;
    float ds = tMax / n;
    vec3 sumR(0), sumM(0), od(0), msR(0), rR, rM;
    for (int i = 0; i < n; ++i) {
        vec3 p = o + dir * ((i + 0.5f) * ds);
        float h = glm::length(p) - kRe;
        vec3 ext = a.extinction(h, rR, rM);
        od += ext * ds * 0.5f;
        vec3 tView = glm::exp(-od);
        vec3 tSun = transmittanceToSun(a, p, sun);
        sumR += rR * tView * tSun * ds;
        sumM += rM * tView * tSun * ds;
        msR += (rR + rM) * tView * ds;   // for the multiple-scattering estimate
        od += ext * ds * 0.5f;
    }
    float mu = glm::dot(dir, sun);
    float phaseR = 3.0f / (16.0f * 3.14159265f) * (1.0f + mu * mu);
    const float g = 0.76f;
    float phaseM = 3.0f / (8.0f * 3.14159265f) * ((1 - g * g) * (1 + mu * mu)) / ((2 + g * g) * std::pow(1 + g * g - 2 * g * mu, 1.5f));
    // multiple scattering: an isotropic term proportional to the light the
    // atmosphere above receives (keeps the zenith blue and the twilight lit)
    float sunUp = glm::clamp(sun.z * 4.0f + 0.25f, 0.0f, 1.0f);
    vec3 ms = msR * transmittanceToSun(a, o + vec3(0, 0, 2000), sun) * (0.35f * sunUp / (4.0f * 3.14159265f));
    return kSunRadiance * (sumR * phaseR + sumM * phaseM + ms);
}

// real SH basis, L2
void shBasis(vec3 d, float out[9]) {
    out[0] = 0.282095f;
    out[1] = 0.488603f * d.y; out[2] = 0.488603f * d.z; out[3] = 0.488603f * d.x;
    out[4] = 1.092548f * d.x * d.y; out[5] = 1.092548f * d.y * d.z; out[6] = 0.315392f * (3 * d.z * d.z - 1);
    out[7] = 1.092548f * d.x * d.z; out[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}
}  // namespace

SkyData computeSky(const Environment& env) {
    Atmos a;
    a.betaR = vec3(5.8e-6f, 13.5e-6f, 33.1e-6f) * env.airDensity;
    a.betaM = vec3(21e-6f) * env.aerosolDensity;
    a.betaO = vec3(0.65e-6f, 1.88e-6f, 0.085e-6f) * env.ozoneDensity;
    a.mieExt = 1.11f;
    SkyData sky;
    vec3 sun = glm::normalize(env.sunDir);
    float sunAz = std::atan2(sun.y, sun.x);
    sky.lut.resize((size_t)sky.width * sky.height * 4);
    float scale = env.skyStrength;
    for (uint32_t y = 0; y < sky.height; ++y) {
        float v = (y + 0.5f) / sky.height;
        float s = v * 2.0f - 1.0f;
        float el = (s < 0 ? -1.0f : 1.0f) * s * s * 0.5f * 3.14159265f;
        for (uint32_t x = 0; x < sky.width; ++x) {
            float az = (x + 0.5f) / sky.width * 2.0f * 3.14159265f + sunAz;
            vec3 dir(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el));
            vec3 L = skyRadiance(a, dir, sun) * scale;
            size_t i = ((size_t)y * sky.width + x) * 4;
            sky.lut[i + 0] = glm::packHalf1x16(L.r);
            sky.lut[i + 1] = glm::packHalf1x16(L.g);
            sky.lut[i + 2] = glm::packHalf1x16(L.b);
            sky.lut[i + 3] = glm::packHalf1x16(1.0f);
        }
    }
    // SH projection of the whole sphere: sky above, lit ground below
    vec3 coeff[9] = {};
    vec3 skyDown(0.0f);
    const int N = 64;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N * 2; ++j) {
            float th = (i + 0.5f) / N * 0.5f * 3.14159265f, ph = (j + 0.5f) / (N * 2) * 2.0f * 3.14159265f;
            vec3 d(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
            skyDown += skyRadiance(a, d, sun) * scale * d.z * std::sin(th) * (0.5f * 3.14159265f / N) * (2.0f * 3.14159265f / (N * 2));
        }
    vec3 groundE = env.sunColor * env.sunStrength * std::max(sun.z, 0.0f) + skyDown;
    vec3 groundL = vec3(0.12f, 0.13f, 0.08f) / 3.14159265f * groundE;
    float wsum = 0.0f;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N * 2; ++j) {
            float th = (i + 0.5f) / N * 3.14159265f, ph = (j + 0.5f) / (N * 2) * 2.0f * 3.14159265f;
            vec3 d(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
            vec3 L = d.z > 0 ? skyRadiance(a, d, sun) * scale : groundL;
            float w = std::sin(th) * (3.14159265f / N) * (2.0f * 3.14159265f / (N * 2));
            float b[9];
            shBasis(d, b);
            for (int k = 0; k < 9; ++k) coeff[k] += L * b[k] * w;
            wsum += w;
        }
    for (int k = 0; k < 9; ++k) sky.sh[k] = vec4(coeff[k], 0.0f);
    return sky;
}
}  // namespace df
