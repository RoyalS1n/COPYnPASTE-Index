#include "render/SkyModel.h"
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cmath>

// Multiple scattering follows S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique" (EGSR 2020) and its MIT-licensed reference implementation (github.com/sebh/UnrealEngineSkyAtmosphere,
// Copyright (c) 2020 Epic Games, Inc.; see THIRD_PARTY.md): a transmittance table, a multiple-scattering table
// Psi_ms(height, sun angle) from second-order scattering summed as a geometric series, and in-scattering
// integrated per step with Psi_ms * scattering added to single scattering. Blender's MULTIPLE_SCATTERING sky
// (the reference renders) is the same family of model, which is why the older single-scattering sky looked too
// saturated next to them.

namespace df {
namespace {
constexpr float kPi = 3.14159265f;
constexpr float kRe = 6360e3f, kRa = 6420e3f, kHr = 8000.0f, kHm = 1200.0f;
// Calibrated against the reference-world Blender renders: a strength-1 sky
// texture (zenith ~2 blue, horizon ~6-8 at low sun); the map's sky strength scales it.
constexpr float kSunRadiance = 200.0f;  // matched to Blender's MULTIPLE_SCATTERING sky texture (strength 1) at 0-80 degrees up

float raySphere(vec3 o, vec3 d, float r) {   // far intersection, -1 if none
    float b = glm::dot(o, d), c = glm::dot(o, o) - r * r, disc = b * b - c;
    if (disc < 0) return -1.0f;
    return -b + std::sqrt(disc);
}
float rayGround(vec3 o, vec3 d) {   // near intersection with the planet, -1 if none
    float b = glm::dot(o, d), c = glm::dot(o, o) - kRe * kRe, disc = b * b - c;
    if (disc < 0) return -1.0f;
    float t = -b - std::sqrt(disc);
    return t > 0 ? t : -1.0f;
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

float phaseRayleigh(float mu) { return 3.0f / (16.0f * kPi) * (1.0f + mu * mu); }
float phaseMie(float mu) {
    const float g = 0.76f;
    return 3.0f / (8.0f * kPi) * ((1 - g * g) * (1 + mu * mu)) / ((2 + g * g) * std::pow(1 + g * g - 2 * g * mu, 1.5f));
}

// Tables over (cos of the sun's zenith angle in [-1, 1]) x (height in [0, top]), bilinear lookups.
struct Table {
    int w = 0, h = 0;
    std::vector<vec3> v;
    vec3 at(float mu, float height) const {
        float x = std::clamp((mu * 0.5f + 0.5f) * (w - 1), 0.0f, w - 1.001f);
        float y = std::clamp(height / (kRa - kRe) * (h - 1), 0.0f, h - 1.001f);
        int x0 = (int)x, y0 = (int)y;
        float tx = x - x0, ty = y - y0;
        auto p = [&](int i, int j) { return v[(size_t)j * w + i]; };
        return glm::mix(glm::mix(p(x0, y0), p(x0 + 1, y0), tx), glm::mix(p(x0, y0 + 1), p(x0 + 1, y0 + 1), tx), ty);
    }
};

vec3 transmittance(const Atmos& a, float height, float mu) {
    vec3 o(0, 0, kRe + height), d(std::sqrt(std::max(0.0f, 1 - mu * mu)), 0, mu);
    if (rayGround(o, d) > 0) return vec3(0.0f);
    float t = raySphere(o, d, kRa);
    const int n = 40;
    vec3 od(0.0f), rR, rM;
    for (int i = 0; i < n; ++i) od += a.extinction(glm::length(o + d * ((i + 0.5f) * t / n)) - kRe, rR, rM) * (t / n);
    return glm::exp(-od);
}

struct Scatter { vec3 L{0.0f}, fms{0.0f}; };

// In-scattered light along a ray. iso: isotropic phase and no multiple scattering (building Psi_ms);
// otherwise Rayleigh / Mie phases plus Psi_ms. Ground hits add sunlight bounced off the planet.
Scatter integrate(const Atmos& a, const Table& trans, const Table* ms, vec3 o, vec3 d, vec3 sun, bool iso, float albedo, int n) {
    Scatter r;
    float tTop = raySphere(o, d, kRa), tGround = rayGround(o, d);
    float tMax = tGround > 0 ? tGround : tTop;
    if (tMax <= 0) return r;
    float mu = glm::dot(d, sun);
    float pR = iso ? 1.0f / (4 * kPi) : phaseRayleigh(mu), pM = iso ? 1.0f / (4 * kPi) : phaseMie(mu);
    vec3 throughput(1.0f), rR, rM;
    float t0 = 0.0f;
    for (int i = 0; i < n; ++i) {
        // quadratic steps: dense near the viewer, where the haze and most of the air are
        float t1 = tMax * ((i + 1.0f) / n) * ((i + 1.0f) / n), dt = t1 - t0;
        vec3 p = o + d * (0.5f * (t0 + t1));
        t0 = t1;
        float rad = glm::length(p), h = rad - kRe;
        vec3 up = p / rad;
        float muS = glm::dot(up, sun);
        vec3 ext = glm::max(a.extinction(h, rR, rM), vec3(1e-12f));
        vec3 sampleT = glm::exp(-ext * dt);
        vec3 tSun = trans.at(muS, h);
        vec3 scat = rR + rM;
        vec3 S = (rR * pR + rM * pM) * tSun;
        if (ms) S += ms->at(muS, h) * scat;
        r.L += throughput * (S - S * sampleT) / ext;              // exact over the step (Frostbite / Hillaire)
        r.fms += throughput * (scat - scat * sampleT) / ext;
        throughput *= sampleT;
    }
    if (tGround > 0) {
        vec3 p = o + d * tGround;
        vec3 up = glm::normalize(p);
        r.L += trans.at(glm::dot(up, sun), 0.0f) * throughput * std::max(glm::dot(up, sun), 0.0f) * albedo / kPi;
    }
    return r;
}

struct SkyAtmosphere {
    Atmos a;
    Table trans, ms;
    bool multiple = true;
    float albedo = 0.3f;
};

void buildTables(SkyAtmosphere& m) {
    m.trans.w = 128; m.trans.h = 64;
    m.trans.v.resize((size_t)m.trans.w * m.trans.h);
    for (int j = 0; j < m.trans.h; ++j)
        for (int i = 0; i < m.trans.w; ++i)
            m.trans.v[(size_t)j * m.trans.w + i] = transmittance(m.a, (float)j / (m.trans.h - 1) * (kRa - kRe),
                                                                 (float)i / (m.trans.w - 1) * 2.0f - 1.0f);
    if (!m.multiple) return;
    // Psi_ms: second-order light gathered over the sphere with an isotropic phase, every higher order as the
    // geometric series 1 / (1 - f_ms) (Hillaire 2020, eq. 5-10)
    m.ms.w = 32; m.ms.h = 32;
    m.ms.v.resize((size_t)m.ms.w * m.ms.h);
    const int sq = 8;
    for (int j = 0; j < m.ms.h; ++j)
        for (int i = 0; i < m.ms.w; ++i) {
            float muS = (float)i / (m.ms.w - 1) * 2.0f - 1.0f;
            float h = std::max((float)j / (m.ms.h - 1) * (kRa - kRe), 1.0f);
            vec3 sun(std::sqrt(std::max(0.0f, 1 - muS * muS)), 0, muS), o(0, 0, kRe + h);
            vec3 L2(0.0f), f(0.0f);
            for (int k = 0; k < sq * sq; ++k) {
                float ra = ((k / sq) + 0.5f) / sq, rb = ((k % sq) + 0.5f) / sq;
                float th = 2 * kPi * ra, ph = std::acos(1 - 2 * rb);
                vec3 d(std::cos(th) * std::sin(ph), std::sin(th) * std::sin(ph), std::cos(ph));
                Scatter s = integrate(m.a, m.trans, nullptr, o, d, sun, true, m.albedo, 20);
                L2 += s.L;
                f += s.fms;
            }
            L2 *= (4 * kPi / (sq * sq)) / (4 * kPi);
            f *= (4 * kPi / (sq * sq)) / (4 * kPi);
            m.ms.v[(size_t)j * m.ms.w + i] = L2 / glm::max(vec3(1.0f) - f, vec3(1e-3f));
        }
}

vec3 skyRadianceSingle(const Atmos& a, vec3 dir, vec3 sun);   // the older model, kept for maps tuned with it

vec3 skyRadiance(const SkyAtmosphere& m, vec3 dir, vec3 sun) {
    if (dir.z < -0.02f) {   // below the horizon: use the horizon at the same azimuth (the terrain hides the rest)
        vec2 h(dir.x, dir.y);
        h = glm::length(h) > 1e-6f ? glm::normalize(h) : vec2(1, 0);
        dir = glm::normalize(vec3(h * 0.9998f, -0.02f));
    }
    if (!m.multiple) return skyRadianceSingle(m.a, dir, sun);
    vec3 o(0, 0, kRe + 100.0f);
    return kSunRadiance * integrate(m.a, m.trans, &m.ms, o, dir, sun, false, m.albedo, 40).L;
}

// ---------------------------------------------------------------- the older single-scattering sky
vec3 transmittanceToSun(const Atmos& a, vec3 p, vec3 s) {
    if (rayGround(p, s) > 0) return vec3(0.0f);
    float t = raySphere(p, s, kRa);
    const int n = 8;
    float ds = t / n;
    vec3 od(0.0f), rR, rM;
    for (int i = 0; i < n; ++i) od += a.extinction(glm::length(p + s * ((i + 0.5f) * ds)) - kRe, rR, rM) * ds;
    return glm::exp(-od);
}

vec3 skyRadianceSingle(const Atmos& a, vec3 dir, vec3 sun) {
    vec3 o(0, 0, kRe + 100.0f);
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
        msR += (rR + rM) * tView * ds;
        od += ext * ds * 0.5f;
    }
    float mu = glm::dot(dir, sun);
    // multiple scattering: an isotropic term proportional to the light the atmosphere above receives
    float sunUp = glm::clamp(sun.z * 4.0f + 0.25f, 0.0f, 1.0f);
    vec3 ms = msR * transmittanceToSun(a, o + vec3(0, 0, 2000), sun) * (0.35f * sunUp / (4.0f * kPi));
    return kSunRadiance * (sumR * phaseRayleigh(mu) + sumM * phaseMie(mu) + ms);
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
    SkyAtmosphere m;
    m.a.betaR = vec3(5.8e-6f, 13.5e-6f, 33.1e-6f) * env.airDensity;
    m.a.betaM = vec3(21e-6f) * env.aerosolDensity;
    m.a.betaO = vec3(0.65e-6f, 1.88e-6f, 0.085e-6f) * env.ozoneDensity;
    m.a.mieExt = 1.11f;
    m.multiple = env.skyMultipleScattering;
    m.albedo = env.groundAlbedo;
    buildTables(m);
    SkyData sky;
    vec3 sun = glm::normalize(env.sunDir);
    float sunAz = std::atan2(sun.y, sun.x);
    sky.lut.resize((size_t)sky.width * sky.height * 4);
    float scale = env.skyStrength;
    for (uint32_t y = 0; y < sky.height; ++y) {
        float v = (y + 0.5f) / sky.height;
        float s = v * 2.0f - 1.0f;
        float el = (s < 0 ? -1.0f : 1.0f) * s * s * 0.5f * kPi;
        for (uint32_t x = 0; x < sky.width; ++x) {
            float az = (x + 0.5f) / sky.width * 2.0f * kPi + sunAz;
            vec3 dir(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el));
            vec3 L = skyRadiance(m, dir, sun) * scale;
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
            float th = (i + 0.5f) / N * 0.5f * kPi, ph = (j + 0.5f) / (N * 2) * 2.0f * kPi;
            vec3 d(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
            skyDown += skyRadiance(m, d, sun) * scale * d.z * std::sin(th) * (0.5f * kPi / N) * (2.0f * kPi / (N * 2));
        }
    vec3 groundE = env.sunColor * env.sunStrength * std::max(sun.z, 0.0f) + skyDown;
    vec3 groundL = vec3(0.12f, 0.13f, 0.08f) / kPi * groundE;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N * 2; ++j) {
            float th = (i + 0.5f) / N * kPi, ph = (j + 0.5f) / (N * 2) * 2.0f * kPi;
            vec3 d(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
            vec3 L = d.z > 0 ? skyRadiance(m, d, sun) * scale : groundL;
            float w = std::sin(th) * (kPi / N) * (2.0f * kPi / (N * 2));
            float b[9];
            shBasis(d, b);
            for (int k = 0; k < 9; ++k) coeff[k] += L * b[k] * w;
        }
    for (int k = 0; k < 9; ++k) sky.sh[k] = vec4(coeff[k], 0.0f);
    return sky;
}
}  // namespace df
