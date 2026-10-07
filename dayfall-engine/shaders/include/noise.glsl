// Value/gradient noise and fbm (world-space procedural shading)
vec3 hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx) * 2.0 - 1.0;
}
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float gnoise(vec3 p) {   // gradient noise, about [-1, 1]
    vec3 i = floor(p), f = fract(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(mix(dot(hash33(i + vec3(0, 0, 0)), f - vec3(0, 0, 0)), dot(hash33(i + vec3(1, 0, 0)), f - vec3(1, 0, 0)), u.x),
                   mix(dot(hash33(i + vec3(0, 1, 0)), f - vec3(0, 1, 0)), dot(hash33(i + vec3(1, 1, 0)), f - vec3(1, 1, 0)), u.x), u.y),
               mix(mix(dot(hash33(i + vec3(0, 0, 1)), f - vec3(0, 0, 1)), dot(hash33(i + vec3(1, 0, 1)), f - vec3(1, 0, 1)), u.x),
                   mix(dot(hash33(i + vec3(0, 1, 1)), f - vec3(0, 1, 1)), dot(hash33(i + vec3(1, 1, 1)), f - vec3(1, 1, 1)), u.x), u.y), u.z) * 1.6;
}
float fbm(vec3 p, int octaves) {   // [0, 1] like Blender's noise texture Fac
    float a = 0.5, s = 0.0, n = 0.0;
    for (int i = 0; i < octaves; ++i) {
        s += a * gnoise(p);
        n += a;
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        a *= 0.55;
    }
    return clamp(0.5 + 0.5 * s / n, 0.0, 1.0);
}
float voronoiEdge(vec2 p) {   // distance to the nearest cell edge
    vec2 i = floor(p), f = fract(p);
    vec2 mr; float md = 8.0; vec2 mg;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        vec2 g = vec2(x, y);
        vec2 o = vec2(hash12(i + g), hash12(i + g + 17.3));
        vec2 r = g + o - f;
        float d = dot(r, r);
        if (d < md) { md = d; mr = r; mg = g; }
    }
    md = 8.0;
    for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) {
        vec2 g = mg + vec2(x, y);
        vec2 o = vec2(hash12(i + g), hash12(i + g + 17.3));
        vec2 r = g + o - f;
        if (dot(mr - r, mr - r) > 0.00001) md = min(md, dot(0.5 * (mr + r), normalize(r - mr)));
    }
    return md;
}
float smoothrange(float v, float a, float b) { return clamp((v - a) / (b - a), 0.0, 1.0); }
