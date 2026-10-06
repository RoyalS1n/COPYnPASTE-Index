#pragma once
// Deterministic gradient noise for terrain and scatter, ported from the
// reference-world Blender pipeline (worldgen/noise.py).
#include <cstdint>

namespace df {
struct Rng {   // splitmix64
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float uniform() { return (float)((next() >> 40) * (1.0 / 16777216.0)); }   // [0, 1)
    float uniform(float a, float b) { return a + (b - a) * uniform(); }
};

class Perlin2D {
public:
    explicit Perlin2D(uint64_t seed);
    float operator()(float x, float y) const;   // roughly [-1, 1]
private:
    uint8_t perm_[512];
    float gx_[256], gy_[256];
    float ox_, oy_;
};

float fbm(const Perlin2D& n, float x, float y, int octaves = 6, float lacunarity = 2.0f, float gain = 0.5f);
// ridged multifractal in [0, 1]: detail concentrates on crests
float ridged(const Perlin2D& n, float x, float y, int octaves = 7, float lacunarity = 2.03f, float gain = 0.5f, float sharpness = 2.0f);
float smoothstep(float e0, float e1, float x);
uint32_t hash32(uint32_t x);
float hash01(uint32_t a, uint32_t b = 0, uint32_t c = 0);
}  // namespace df
