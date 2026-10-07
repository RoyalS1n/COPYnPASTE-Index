// Shading models. Each fills a Surface from the material, the interpolated
// vertex data and world-space procedural detail; light() then lights it.
const uint NO_TEX = 0xFFFFFFFFu;

struct Surface {
    vec3 albedo;
    vec3 n;
    float rough;
    float metal;
    float spec;          // specular level (0.5 = F0 0.04)
    float translucency;
    vec3 emissive;
    float alpha;
    float ao;
};

vec4 tex(uint idx, vec2 uv) { return texture(textures[nonuniformEXT(idx)], uv); }

Surface defaultSurface(vec3 n) {
    Surface s;
    s.albedo = vec3(0.5); s.n = n; s.rough = 0.8; s.metal = 0.0; s.spec = 0.5;
    s.translucency = 0.0; s.emissive = vec3(0.0); s.alpha = 1.0; s.ao = 1.0;
    return s;
}

// glTF metallic-roughness (Blender / Unreal / Meshy exports)
Surface shadeLit(Material m, vec3 n, vec4 t, vec2 uv, vec4 color0) {
    Surface s = defaultSurface(n);
    vec4 base = m.c[0];
    if (m.h0.z != NO_TEX) base *= tex(m.h0.z, uv);
    if ((m.h0.y & MAT_VERTEXCOLOR) != 0u) base *= color0;
    s.albedo = base.rgb;
    s.alpha = base.a;
    s.rough = m.p[0].x;
    s.metal = m.p[0].y;
    s.spec = m.p[0].z;
    if (m.h1.x != NO_TEX) {
        vec3 orm = tex(m.h1.x, uv).rgb;
        s.ao = mix(1.0, orm.r, m.p[0].w);
        s.rough *= orm.g;
        s.metal *= orm.b;
    }
    if (m.h0.w != NO_TEX && dot(t.xyz, t.xyz) > 0.0) {
        vec3 tn = tex(m.h0.w, uv).xyz * 2.0 - 1.0;
        tn.xy *= m.p[1].w > 0.0 ? m.p[1].w : 1.0;
        vec3 T = normalize(t.xyz - n * dot(n, t.xyz));
        vec3 B = cross(n, T) * t.w;
        s.n = normalize(T * tn.x + B * tn.y + n * tn.z);
    }
    vec3 e = m.c[1].rgb * m.c[1].a;
    if (m.h1.y != NO_TEX) e *= tex(m.h1.y, uv).rgb;
    s.emissive = e;
    return s;
}

// Terrain: biome weights in the vertex colours (R grass, G rock, B snow, A wet;
// colour1 R path, G dry, B lane), lane distance in uv1.x, world-space detail.
Surface shadeTerrain(Material m, vec3 p, vec3 n, vec4 c0, vec4 c1, vec2 uv1) {
    Surface s = defaultSurface(n);
    float grassM = c0.r, rockM = c0.g, snowM = c0.b, wet = c0.a, path = c1.r, dryM = c1.g, laneOn = c1.b;
    float large = fbm(p * 0.004, 4), mid = fbm(p * 0.05, 5), fine = fbm(p * 0.9, 4);
    vec3 grass = m.c[0].rgb, grassDry = m.c[1].rgb, soil = m.c[2].rgb, rock = m.c[3].rgb, rockDark = m.c[4].rgb;
    vec3 moss = m.c[5].rgb, snow = m.c[6].rgb;
    float dryF = clamp(dryM + (large - 0.5) * 0.6, 0.0, 1.0);
    vec3 grassCol = mix(grass, grassDry, dryF);
    grassCol = mix(grassCol, soil, smoothrange(mid, 0.35, 0.7));
    float streak = fbm(p * 0.6, 4);
    grassCol = mix(grassCol, grass * 0.55, smoothrange(streak, 0.3, 0.7) * 0.45);
    grassCol = mix(grassCol, vec3(0.30, 0.27, 0.11), smoothrange(fbm(p * 0.015, 3), 0.58, 0.72) * 0.32 * m.p[1].x);
    // rock: distorted strata, tonal patches, cracks
    float strata = sin(p.z * 0.22 + fbm(p * 0.035, 4) * 14.0) * 0.5 + 0.5;
    vec3 rockCol = mix(rock, rockDark, smoothrange(strata, 0.2, 0.9) * 0.45);
    rockCol = mix(rockCol, rock * 0.6, smoothrange(large, 0.3, 0.7) * 0.5);
    float crack = 1.0 - smoothrange(voronoiEdge(p.xy * 0.22 + vec2(p.z * 0.05)), 0.0, 0.03);
    rockCol = mix(rockCol, rockDark, crack * 0.25);
    float steep = smoothrange(0.8 - n.z, 0.0, 0.2);
    float rockF = smoothrange(max(rockM, steep * 0.7) + (mid - 0.5) * 0.8, 0.35, 0.6);
    // ledges inside rock catch moss
    float ledge = smoothrange(n.z, 0.74, 0.88) * smoothrange(fbm(p * 0.09, 4), 0.42, 0.6);
    rockCol = mix(rockCol, mix(moss, grassDry, dryF), ledge * rockF);
    vec3 col = mix(grassCol, rockCol, rockF);
    float snowF = smoothrange(snowM + (fine - 0.5) * 0.6, 0.4, 0.55);
    col = mix(col, snow, snowF);
    // wet ground: dark silty mud; below the waterline: silt, algae, leaf litter
    vec3 mud = mix(soil * 0.32, soil * 0.62, smoothrange(fbm(p * 0.35, 4), 0.35, 0.65));
    col = mix(col, mud, wet);
    float wl = m.p[0].x;
    float under = smoothrange(p.z, wl + 0.03, wl - 0.12);
    vec3 silt = mix(vec3(0.05, 0.043, 0.03), vec3(0.035, 0.055, 0.018), smoothrange(fbm(p * 0.5, 4), 0.42, 0.68));
    col = mix(col, silt, under);
    // roads and the lane's two wheel ruts (drawn per pixel from the lane distance)
    float rut = smoothrange(abs(abs(uv1.x) - 0.78), 0.34, 0.14);
    float earthF = path * ((1.0 - laneOn) + laneOn * rut);
    vec3 earth = mix(rock * 1.6, soil * 1.35, smoothrange(fbm(p * 9.0, 2), 0.3, 0.7));
    earth = mix(earth, soil, smoothrange(mid, 0.4, 0.65) * 0.5);
    col = mix(col, earth, smoothrange(earthF, 0.2, 0.7));
    s.albedo = col;
    float puddle = wet * wet * smoothrange(fbm(p * 0.8, 3), 0.48, 0.66);
    s.rough = max(0.92 - wet * 0.42 - puddle * 0.4, 0.07);
    s.spec = 0.24 + wet * 0.66;
    float h = fine + 2.0 * mid - crack * rockF * 1.5 - rut * laneOn * path * 2.0;
    s.n = perturbNormal(n, p, h * 0.12, 1.0);
    return s;
}

// Leaves, needles, reeds, ivy: per-instance variation, per-vertex shading
// (colour0.r = leafvar / 1.5 when MAT_VERTEXCOLOR), warm tips, translucency.
Surface shadeFoliage(Material m, vec3 n, vec2 uv, vec4 c0, float seed) {
    Surface s = defaultSurface(n);
    vec3 col = mix(m.c[0].rgb, m.c[1].rgb, seed * m.p[0].z);
    float lv = (m.h0.y & MAT_VERTEXCOLOR) != 0u ? c0.r * 1.5 : 1.0;
    col *= lv * mix(0.85, 1.15, fract(seed * 7.13));
    col = mix(col, m.c[2].rgb, smoothrange(lv, 1.0, 1.45) * 0.55);
    float a = 1.0;
    if (m.h0.z != NO_TEX) { vec4 t = tex(m.h0.z, uv); col *= t.rgb; a = t.a; }
    s.albedo = col;
    s.alpha = a * m.c[0].a;
    s.rough = m.p[0].x;
    s.translucency = m.p[0].y;
    s.spec = 0.4;
    return s;
}

// Grass blades: uv0.y runs root (0) -> tip (1); dry patches follow the
// instance position so they match the terrain underneath.
Surface shadeGrass(Material m, vec3 n, vec2 uv, vec4 inst) {
    Surface s = defaultSurface(n);
    float dry = smoothrange(fbm(inst.xyz * 0.004, 4), 0.35, 0.65) * 0.9 + (inst.w - 0.5) * 0.3;
    vec3 base = mix(m.c[0].rgb, m.c[1].rgb, clamp(dry, 0.0, 1.0));
    vec3 root = base * 0.3, tip = base * vec3(1.0, 0.95, 0.6);
    vec3 col = mix(root, base, clamp(uv.y, 0.0, 1.0));
    col = mix(col, mix(base, tip, 0.35), smoothrange(uv.y, 0.7, 1.0));
    s.albedo = col;
    s.rough = 0.55;
    s.translucency = 0.4;
    s.spec = 0.45;
    return s;
}

// Coursed stone, roof tiles and planks on metre-scale UVs: per-block tone,
// recessed mortar, weathering, grime and moss at the foot (uv0.y = height).
Surface shadeCourses(Material m, vec3 p, vec3 n, vec2 uv) {
    Surface s = defaultSurface(n);
    vec2 sz = max(m.p[0].xy, vec2(0.01));
    vec2 q = uv / sz;
    float row = floor(q.y);
    q.x += fract(row * m.p[0].w);
    vec2 cell = floor(q), f = fract(q);
    vec2 d = min(f, 1.0 - f) * sz;
    float e = min(d.x, d.y);
    float mortarW = m.p[0].z;
    float mortar = 1.0 - smoothrange(e, mortarW * 0.5, mortarW * 0.5 + 0.012);
    float rnd = hash12(cell);
    vec3 col = mix(m.c[1].rgb, m.c[0].rgb, rnd);
    col *= mix(0.75, 1.1, fbm(p * 0.06, 4));
    col = mix(col, m.c[0].rgb * 0.6, smoothrange(fbm(vec3(uv.x * 2.5, uv.y * 0.08, 0.0), 3), 0.45, 0.72) * 0.6);
    col = mix(col, m.c[2].rgb, mortar);
    float foot = clamp(1.0 - uv.y / 3.5, 0.0, 1.0);
    col = mix(col, m.c[3].rgb, foot * m.p[1].x * smoothrange(fbm(p * 0.5, 3), 0.4, 0.65));
    s.albedo = col;
    s.rough = mix(m.p[1].y, 0.95, mortar);
    s.spec = m.p[1].w;
    float h = smoothrange(e, mortarW * 0.5, mortarW * 0.5 + 0.04) + fbm(p * 6.0, 3) * 0.3;
    s.n = perturbNormal(n, p, h * m.p[1].z, 1.0);
    return s;
}

// Boulders and outcrops: world-space strata, tonal patches and cracks, moss on
// upward faces and lichen spots (the reference-world RW_Rock shader).
Surface shadeRock(Material m, vec3 p, vec3 n) {
    Surface s = defaultSurface(n);
    vec3 rock = m.c[0].rgb, dark = m.c[1].rgb, moss = m.c[2].rgb, lichen = m.c[3].rgb;
    float large = fbm(p * 0.22, 4), mid = fbm(p * 1.4, 4), fine = fbm(p * 7.0, 3);
    float strata = sin(p.z * 2.6 + fbm(p * 0.5, 4) * 9.0) * 0.5 + 0.5;
    vec3 col = mix(rock, dark, smoothrange(strata, 0.25, 0.95) * 0.3);
    col = mix(col, rock * 0.62, smoothrange(large, 0.32, 0.72) * 0.55);
    col *= mix(0.82, 1.14, fine);
    float crack = 1.0 - smoothrange(voronoiEdge(p.xy * 0.75 + vec2(p.z * 0.6, -p.z * 0.3)), 0.0, 0.07);
    crack *= smoothrange(fbm(p * 0.6 + 1.7, 3), 0.35, 0.6);   // cracks in places, not a net over the whole stone
    col = mix(col, dark * 0.8, crack * 0.32);
    float up = smoothrange(n.z, 0.5, 0.92);
    float mossF = up * smoothrange(fbm(p * 0.8 + 3.1, 4), 0.42, 0.6) * m.p[0].x;
    col = mix(col, moss * mix(0.7, 1.2, fine), mossF);
    float lich = smoothrange(fbm(p * 3.1 + 7.7, 3), 0.66, 0.72) * m.p[0].y * (1.0 - mossF);
    col = mix(col, lichen, lich);
    s.albedo = col;
    s.rough = mix(0.82, 0.96, mossF);
    s.spec = 0.4;
    float h = mid * 0.6 + fine * 0.25 - crack * 0.6;
    s.n = perturbNormal(n, p, h * m.p[0].z, 1.0);
    return s;
}

vec3 envBRDFApprox(vec3 f0, float rough, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

vec3 lightSurface(Surface s, vec3 p, vec3 v, float viewDepth) {
    vec3 L = frame.sunDir.xyz;
    vec3 sunE = frame.sunColor.rgb * frame.sunDir.w;
    float t = s.translucency;
    float shadow = sampleShadow(p, s.n, viewDepth);
    float NoL = dot(s.n, L);
    float NoV = max(dot(s.n, v), 1e-4);
    float a = max(s.rough * s.rough, 0.002);
    vec3 f0 = mix(vec3(0.08 * s.spec), s.albedo, s.metal);
    vec3 diffuseE = sunE * shadow * ((1.0 - t) * max(NoL, 0.0) + t * max(-NoL, 0.0));
    vec3 spec = vec3(0.0);
    if (NoL > 0.0) {
        vec3 h = normalize(v + L);
        spec = sunE * shadow * NoL * D_GGX(max(dot(s.n, h), 0.0), a) * V_Smith(NoV, NoL, a) * F_Schlick(f0, max(dot(v, h), 0.0));
    }
    pointLights(p, s.n, v, a, f0, t, diffuseE, spec);
    vec3 amb = (shIrradiance(s.n) * (1.0 - t) + shIrradiance(-s.n) * t) * s.ao;
    vec3 R = reflect(-v, s.n);
    vec3 envR = mix(skyRadiance(normalize(vec3(R.xy, max(R.z, 0.02)))), shIrradiance(R) / PI, s.rough);
    envR *= smoothrange(R.z, -0.3, 0.1) * 0.8 + 0.2;   // the ground occludes reflections from below
    vec3 specAmb = envR * envBRDFApprox(f0, s.rough, NoV) * s.ao;
    vec3 diff = s.albedo * (1.0 - s.metal) / PI;
    return diff * (diffuseE + amb) + (spec + specAmb) * (1.0 - t * 0.5) + s.emissive;
}
