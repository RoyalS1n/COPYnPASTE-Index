#version 460
#extension GL_GOOGLE_include_directive : require
// Water surfaces (the sea plane, lakes, rivers), shaded as a single layer of participating medium over the opaque
// scene: a spectrum of wind waves (deep-water dispersion) with distance filtering; screen-space reflections of the
// scene, falling back to the sky; refraction with per-channel absorption and single scattering of sun and sky light
// in the water column (the colour deep water takes); caustics on the shallow bottom; foam where the water is thin
// (shores, around objects) and on steep crests; light through the wave tips; a shoreline that fades instead of
// cutting. Rivers flow: their ribbons carry the flow direction in the tangent and its speed (m/s) in colour1.r.
// Marsh water can carry duckweed (MAT_DUCKWEED, colour0.r = depth / 2).
//
// Material: c[0].rgb absorption (1/m), c[2].rgb scattering (1/m), c[3] foam colour + amount, p[0] (-, roughness,
// wave strength, wave scale), p[1].x fetch (m), p[2] (caustics, screen-space reflections on, shore fade (m), foam width (m)).
#include "common.glsl"
#include "noise.glsl"
#include "sky.glsl"
#include "lighting.glsl"
#include "surface.glsl"
layout(location = 0) in vec3 vPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec4 vUv;
layout(location = 4) in vec4 vColor0;
layout(location = 5) in vec4 vColor1;
layout(location = 6) flat in uint vMaterial;
layout(location = 7) flat in vec4 vInst;
layout(location = 0) out vec4 outColor;

float linearDist(float d) { return frame.water.z / max(d, 1e-7); }   // reversed infinite Z: view depth

// screen position of a world point: uv (top-left origin, the viewport is flipped) and view depth
bool toScreen(vec3 q, out vec2 uv, out float depth) {
    vec4 c = frame.viewProj * vec4(q, 1.0);
    depth = c.w;
    uv = vec2(-1.0);
    if (c.w <= 0.05) return false;
    vec2 ndc = c.xy / c.w;
    uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

// wind waves: 14 directional waves spread around the wind. Their wavelengths are fetch-limited (a pond has only
// ripples, the open sea has swell): the longest is about a tenth of the fetch, up to 16 m; each shorter one is 0.7x.
// Speeds follow gravity-capillary dispersion (omega^2 = g k + sigma/rho k^3). Short waves come and go in gusts
// (cat's paws); every wave fades out where a pixel spans a good part of its wavelength, so distant water stays calm.
vec2 waveSlope(vec2 q, float t, vec2 wind, float footprint, float lmax, float gust, out float crest) {
    vec2 g = vec2(0.0);
    crest = 0.0;
    const int N = 14;
    float wa = atan(wind.y, wind.x);
    for (int i = 0; i < N; ++i) {
        float fi = float(i);
        float lambda = lmax * pow(0.7, fi + 0.45 * fract(fi * 0.7548777));      // jittered, so no two waves lock together
        float spread = (fract(fi * 0.618034 + 0.31) - 0.5) * 2.0;             // +-57 degrees around the wind
        vec2 d = vec2(cos(wa + spread), sin(wa + spread));
        float k = 6.2831853 / lambda;
        float w = sqrt(9.81 * k + 7.4e-5 * k * k * k);
        float steep = 0.028 * (1.0 - 0.45 * abs(spread)) * mix(0.45, 1.0, smoothstep(0.0, 4.0, fi)) * mix(1.0, gust, smoothstep(0.0, 5.0, fi));   // slope k * A
        float keep = 1.0 - smoothstep(lambda * 0.05, lambda * 0.3, footprint);
        float ph = k * dot(q, d) - w * t + fi * 1.7;
        // a little Gerstner sharpening: narrow crests, wide troughs
        float c = cos(ph), sh = c + 0.25 * (c * c - 0.5);
        g += d * (steep * sh * keep);
        if (i < 5) crest += max(sh, 0.0) * steep * keep;
    }
    return g;
}

// caustics: two drifting layers of a sharpened cell-edge pattern, multiplied (the bright web under shallow water)
float caustics(vec2 q, float t) {
    float a = voronoiEdge(q * 0.9 + vec2(t * 0.11, t * 0.07));
    float b = voronoiEdge(q * 1.3 - vec2(t * 0.08, -t * 0.13) + 7.3);
    float ca = 1.0 - smoothstep(0.0, 0.16, a), cb = 1.0 - smoothstep(0.0, 0.18, b);
    return ca * cb * 2.5 + (ca + cb) * 0.25;
}

// Henyey-Greenstein phase function (forward-scattering water)
float phaseHG(float c, float g) { float g2 = g * g; return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5)); }

void main() {
    Material m = materials[vMaterial];
    float time = frame.cameraPos.w;
    vec3 p = vPos;
    vec3 cam = frame.cameraPos.xyz;
    vec3 v = normalize(cam - p);
    float camDist = length(cam - p);
    vec3 sigmaA = max(m.c[0].rgb, vec3(1e-4));
    vec3 sigmaS = max(m.c[2].rgb, vec3(0.0));
    vec3 sigmaT = sigmaA + sigmaS;
    float rough = max(m.p[0].y, 0.02);
    float waveStrength = m.p[0].z > 0.0 ? m.p[0].z : 1.0;
    float waveScale = m.p[0].w > 0.0 ? m.p[0].w : 1.0;
    bool useSsr = m.p[2].y > 0.5;
    float shoreFade = max(m.p[2].z, 0.01);
    float foamWidth = m.p[2].w;

    // ---------------------------------------------------------------- waves
    vec2 wind = length(frame.wind.xy) > 1e-3 ? normalize(frame.wind.xy) : vec2(0.8, 0.6);
    float flowSpeed = vColor1.r;
    vec2 flow = vTangent.xy * flowSpeed;
    vec2 q = p.xy - flow * time;
    float footprint = length(fwidth(p.xy));
    float fetch = m.p[1].x > 0.0 ? m.p[1].x : vColor1.g > 0.0 ? vColor1.g : 1e4;   // material, water body size, open sea
    float lmax = clamp(fetch * 0.1, 0.5, 16.0) * waveScale;
    float gust = 0.25 + 1.5 * smoothstep(0.3, 0.75, fbm(vec3(q * 0.045 - wind * time * 0.35, time * 0.03), 3));
    float crest;
    vec2 g;
    if (flowSpeed > 0.0) {
        // a current combs its ripples into streaks along the flow: waves in a frame squashed along the flow
        vec2 t = vTangent.xy, sd = vec2(-t.y, t.x);
        const float squash = 0.35;
        vec2 gl = waveSlope(vec2(dot(q, t) * squash, dot(q, sd)), time, normalize(vec2(0.5, 1.0)), footprint, lmax, gust, crest);
        g = (t * gl.x * squash + sd * gl.y) * waveStrength;
    } else {
        g = waveSlope(q, time, wind, footprint, lmax, gust, crest) * waveStrength;
    }
    // fine capillary detail: the gradient of drifting noise, only where pixels are small enough to show it
    float fine = 1.0 - smoothstep(0.02, 0.12, footprint);
    if (fine > 0.0) {
        vec3 nq = vec3(q * 4.3, time * 0.8);
        float e = 0.04;
        float n0 = gnoise(nq);
        g += vec2(gnoise(nq + vec3(e, 0, 0)) - n0, gnoise(nq + vec3(0, e, 0)) - n0) / e * 0.005 * fine * waveStrength * gust;
    }
    vec3 n = normalize(vec3(-g, 1.0));
    float NoV = max(dot(n, v), 1e-3);

    // ---------------------------------------------------------------- the water column
    vec2 suv = gl_FragCoord.xy * frame.viewport.zw;
    float fragDepth = linearDist(gl_FragCoord.z);
    float sceneDepth0 = linearDist(texture(sceneDepth, suv).r);
    float thick = max(sceneDepth0 - fragDepth, 0.0);                          // view depth through the water
    float rayScale = camDist / max(fragDepth, 1e-3);                            // view depth -> distance along the ray
    vec3 ground = cam + (p - cam) * (sceneDepth0 / max(fragDepth, 1e-3));
    float depthBelow = max(p.z - ground.z, 0.0);                                // vertical depth to the bottom
    // refraction: the bottom seen through the tilted surface, offset more for thicker water
    vec2 ruv = suv + n.xy * 0.035 * clamp(thick, 0.0, 1.5) / max(1.0, fragDepth * 0.04);
    float rthick = linearDist(texture(sceneDepth, ruv).r) - fragDepth;
    if (rthick < 0.0) { ruv = suv; rthick = thick; }                            // a foreground object: no offset
    vec3 refr = texture(sceneColor, ruv).rgb;
    float pathLen = max(rthick, 0.0) * rayScale;

    vec3 L = frame.sunDir.xyz;
    vec3 sunE = frame.sunColor.rgb * frame.sunDir.w;
    float shadow = sampleShadow(p, vec3(0, 0, 1), fragDepth);
    vec3 skyE = shIrradiance(vec3(0, 0, 1));

    // caustics on the bottom: sunlight focused by the waves, strongest in the first metres
    float causticAmt = m.p[2].x;
    if (causticAmt > 0.0 && depthBelow > 0.02) {
        vec2 cq = ground.xy + L.xy / max(L.z, 0.2) * depthBelow * 0.75 - flow * time;
        float fade = smoothstep(0.02, 0.35, depthBelow) * exp(-dot(sigmaT, vec3(0.333)) * depthBelow * 1.6);
        float cfoot = length(fwidth(cq));
        fade *= 1.0 - smoothstep(0.15, 0.6, cfoot);                             // too small to resolve: none
        refr *= 1.0 + caustics(cq * 1.1, time) * causticAmt * fade * shadow * clamp(L.z * 1.5, 0.0, 1.0);
    }

    // absorption along the path, and single scattering of sun and sky light inside the column: light reaching depth
    // h fades with exp(-sigmaT h / mu), the view ray reaches depth h(s) = s * depth / path
    vec3 T = exp(-sigmaT * pathLen);
    float muSun = max(L.z, 0.15);
    float dPerL = depthBelow / max(pathLen, 1e-3);
    float kSun = 1.0 + dPerL / muSun, kSky = 1.0 + dPerL;
    float deep = smoothstep(120.0, 250.0, pathLen);                             // open sea: no bottom in reach
    vec3 sunIn = sunE * shadow * phaseHG(dot(-v, L), 0.75) * (1.0 - exp(-sigmaT * pathLen * kSun)) / (sigmaT * kSun);
    vec3 skyIn = skyE / (4.0 * PI) * (1.0 - exp(-sigmaT * pathLen * kSky)) / (sigmaT * kSky);
    vec3 inscatter = sigmaS * (sunIn + skyIn);
    vec3 under = mix(refr * T, vec3(0.0), deep) + inscatter;

    // light through the wave tips: looking towards the sun through steep crests
    vec2 toSun = normalize(L.xy + 1e-4), toView = normalize(-v.xy + 1e-4);
    float tips = pow(max(dot(toView, toSun), 0.0), 4.0) * clamp(crest * 10.0, 0.0, 1.0) * clamp(1.0 - v.z * 1.5, 0.0, 1.0);
    under += sigmaS / sigmaT * sunE * shadow * tips * 0.08;

    // ---------------------------------------------------------------- reflection
    vec3 R = reflect(-v, n);
    R.z = max(R.z, 0.005);
    R = normalize(R);
    vec3 refl = skyRadiance(R);
    vec4 cl = cloudLayer(p, R);
    refl = (refl * (1.0 - cl.a) + cl.rgb) * skyVisibility(skyOcclusion(p, vec3(0, 0, 1)), R);
    if (useSsr) {
        // march the reflected ray through the depth buffer; the hit shows the scene, misses the sky
        vec3 ro = p + vec3(0, 0, 0.02);
        float t = 0.15, stepLen = max(0.08, camDist * 0.012);
        float prevT = 0.0;
        bool hit = false;
        vec2 huv = vec2(0.0);
        for (int i = 0; i < 48 && !hit; ++i) {
            vec3 x = ro + R * t;
            vec2 uv;
            float rd;
            if (!toScreen(x, uv, rd)) break;
            float sd = linearDist(textureLod(sceneDepth, uv, 0.0).r);
            float diff = rd - sd;
            if (diff > 0.0 && diff < max(stepLen * 2.5, 0.35)) {   // behind a surface, but not far behind
                // refine between the last miss and this hit
                float a = prevT, b = t;
                for (int k = 0; k < 6; ++k) {
                    float mid = 0.5 * (a + b);
                    vec2 muv;
                    float md;
                    toScreen(ro + R * mid, muv, md);
                    if (md - linearDist(textureLod(sceneDepth, muv, 0.0).r) > 0.0) { b = mid; huv = muv; } else a = mid;
                }
                if (huv == vec2(0.0)) huv = uv;
                hit = true;
            }
            prevT = t;
            t += stepLen;
            stepLen *= 1.07;
        }
        if (hit) {
            vec2 edge = smoothstep(vec2(0.0), vec2(0.08), huv) * smoothstep(vec2(0.0), vec2(0.08), 1.0 - huv);
            float conf = edge.x * edge.y;
            refl = mix(refl, textureLod(sceneColor, huv, 0.0).rgb, conf);
        }
    }

    // ---------------------------------------------------------------- the surface
    float fx = clamp(1.0 - NoV, 0.0, 1.0);
    float F = 0.02 + 0.98 * fx * fx * fx * fx * fx;
    vec3 h = normalize(v + L);
    float a = max(rough * rough, 0.0015);
    float NoL = max(dot(n, L), 0.0);
    vec3 glint = sunE * shadow * NoL * D_GGX(max(dot(n, h), 0.0), a) * V_Smith(NoV, NoL, a) * F;
    // a soft shoreline: where the water is only a film the surface fades into the bottom instead of cutting
    float shore = smoothstep(0.0, shoreFade, depthBelow) * smoothstep(0.0, shoreFade * 0.5, thick);
    vec3 col = mix(under, refl, F * shore) + glint * shore;
    col = mix(refr, col, max(shore, deep));

    // foam: where the water thins out (shores, around objects), broken up by drifting noise, with a few lapping
    // bands; and a little on steep crests
    float foamAmt = m.c[3].a;
    if (foamAmt > 0.0 && foamWidth > 0.0) {
        float thin = 1.0 - smoothstep(0.0, foamWidth, min(depthBelow, thick * 0.7));
        float lap = 0.7 + 0.3 * sin(min(depthBelow, thick * 0.7) / max(foamWidth, 0.02) * 6.0 - time * 1.4);
        float breakup = smoothrange(fbm(vec3(q * 0.8, time * 0.08), 3) * 0.6 + fbm(vec3(q * 3.7, time * 0.2), 3) * 0.4, 0.4, 0.62);
        // breaking needs energy: wave height grows with fetch, and a current piles up foam against what it meets
        float energy = clamp(waveStrength * gust * smoothstep(1.0, 12.0, lmax) + flowSpeed * 0.35, 0.0, 1.5);
        float edge = smoothstep(0.0, foamWidth * 0.35, min(depthBelow, thick * 0.7));   // a band just off the waterline
        float foam = clamp(thin * edge * (0.3 + 0.7 * lap) * breakup * 1.5 * energy, 0.0, 1.0);
        foam = max(foam, smoothrange(crest * waveStrength * gust, 0.1, 0.16) * breakup * 0.7);
        foam *= foamAmt * (1.0 - smoothstep(60.0, 220.0, camDist));
        if (foam > 0.001) {
            Surface s = defaultSurface(normalize(n + vec3(0, 0, 1)));
            s.albedo = m.c[3].rgb;
            s.rough = 0.9;
            col = mix(col, lightSurface(s, p, v, fragDepth), foam);
        }
    }

    if ((m.h0.y & MAT_DUCKWEED) != 0u) {
        float depth = vColor0.r * 2.0;
        float shallow = smoothrange(depth, 0.55, 0.08);
        float patches = smoothrange(fbm(p * 0.16, 4), 0.45, 0.6);
        float speck = smoothrange(voronoiEdge(p.xy * 90.0), 0.0, 0.25);
        float weed = smoothrange(shallow * patches * (0.45 + 0.55 * speck), 0.25, 0.6);
        Surface s = defaultSurface(vec3(0, 0, 1));
        s.albedo = mix(m.c[1].rgb * 0.4, m.c[1].rgb, speck);
        s.rough = 0.55;
        vec3 weedCol = lightSurface(s, p, v, fragDepth);
        col = mix(col, weedCol, weed);
    }
    outColor = vec4(applyHaze(col, p), 1.0);
}
