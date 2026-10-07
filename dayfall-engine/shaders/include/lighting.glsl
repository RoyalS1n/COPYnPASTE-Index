// Shading helpers: GGX, sky irradiance (SH), sky occlusion, cascaded shadows, point lights, haze.
float D_GGX(float NoH, float a) { float a2 = a * a; float f = (NoH * a2 - NoH) * NoH + 1.0; return a2 / (PI * f * f); }
float V_Smith(float NoV, float NoL, float a) {
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
vec3 F_Schlick(vec3 f0, float VoH) {
    float x = clamp(1.0 - VoH, 0.0, 1.0);   // VoH can round above 1: pow of a negative base is NaN
    float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

vec3 shIrradiance(vec3 n) {
    const float c1 = 0.429043, c2 = 0.511664, c3 = 0.743125, c4 = 0.886227, c5 = 0.247708;
    vec3 L00 = frame.sh[0].rgb, L1m1 = frame.sh[1].rgb, L10 = frame.sh[2].rgb, L11 = frame.sh[3].rgb;
    vec3 L2m2 = frame.sh[4].rgb, L2m1 = frame.sh[5].rgb, L20 = frame.sh[6].rgb, L21 = frame.sh[7].rgb, L22 = frame.sh[8].rgb;
    float x = n.x, y = n.y, z = n.z;
    return max(c1 * L22 * (x * x - y * y) + c3 * L20 * z * z + c4 * L00 - c5 * L20 + 2.0 * c1 * (L2m2 * x * y + L21 * x * z + L2m1 * y * z)
               + 2.0 * c2 * (L11 * x + L1m1 * y + L10 * z), vec3(0.0));
}

// Sky occlusion around static structures (world/SkyOcclusion.h): an ambient cube, the open fraction of
// the hemisphere around +x -x +y -y (h) and +z -z (v), blended by strength and by the fade at the
// volume's edge. Sampled a little off the surface along n so the cells inside walls are not read.
struct SkyOcc { vec4 h; vec2 v; };
SkyOcc skyOcclusion(vec3 p, vec3 n) {
    SkyOcc o = SkyOcc(vec4(1.0), vec2(1.0));
    uint count = uint(frame.skyOcc.x);
    for (uint i = 0u; i < count; ++i) {
        vec4 org = frame.skyOccRegions[i * 2u], size = frame.skyOccRegions[i * 2u + 1u];
        vec3 g = (p - org.xyz) / org.w + n * frame.skyOcc.z;   // in cells
        if (any(lessThan(g, vec3(0.0))) || any(greaterThan(g, size.xyz))) continue;
        vec3 uvw = vec3(clamp(g.x, 0.5, size.x - 0.5) / (2.0 * size.x), g.yz / size.yz);   // the two halves sit side by side in x
        vec4 h = textureLod(skyVolumes[nonuniformEXT(i)], uvw, 0.0);
        vec2 v = textureLod(skyVolumes[nonuniformEXT(i)], uvw + vec3(0.5, 0.0, 0.0), 0.0).xy;
        vec3 edge = min(g, size.xyz - g);
        float w = frame.skyOcc.y * clamp(min(edge.x, min(edge.y, edge.z)) * 0.5 - 0.5, 0.0, 1.0);
        o.h = mix(vec4(1.0), h, w);
        o.v = mix(vec2(1.0), v, w);
        break;
    }
    return o;
}
// cosine-weighted open fraction of the hemisphere around dir (unit)
float skyVisibility(SkyOcc o, vec3 dir) {
    vec3 d2 = dir * dir;
    return d2.x * (dir.x >= 0.0 ? o.h.x : o.h.y) + d2.y * (dir.y >= 0.0 ? o.h.z : o.h.w) + d2.z * (dir.z >= 0.0 ? o.v.x : o.v.y);
}

float sampleShadow(vec3 p, vec3 n, float viewDepth) {
    int count = int(frame.shadowParams.w);
    if (count == 0) return 1.0;
    int c = 0;
    for (; c < count; ++c) if (viewDepth < frame.cascadeSplits[c]) break;
    if (c >= count) return 1.0;
    float texel = frame.shadowParams.x;
    vec3 sp = p + n * frame.shadowParams.y * (1.0 + float(c));
    vec4 lp = frame.cascadeViewProj[c] * vec4(sp, 1.0);
    vec3 uvz = lp.xyz / lp.w;
    vec2 uv = vec2(uvz.x * 0.5 + 0.5, 0.5 - uvz.y * 0.5);   // 3D passes use a negative viewport height
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    float r = frame.shadowParams.z;
    float s = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            s += texture(shadowMap, vec4(uv + vec2(x, y) * texel * r, float(c), uvz.z));
    s /= 9.0;
    // fade out over the last 10% of the final cascade
    float fade = smoothrange(viewDepth, frame.cascadeSplits[count - 1] * 0.9, frame.cascadeSplits[count - 1]);
    return mix(s, 1.0, fade);
}

// diffuse + specular from the static point-light grid
void pointLights(vec3 p, vec3 n, vec3 v, float a, vec3 f0, float translucency, inout vec3 diffuse, inout vec3 spec) {
    uint count = frame.lightGridDims.w;
    if (count == 0u) return;
    vec3 g = (p - frame.lightGrid.xyz) / frame.lightGrid.w;
    ivec3 cell = ivec3(floor(g));
    uvec3 dims = frame.lightGridDims.xyz;
    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(dims)))) return;
    uint idx = uint(cell.x) + dims.x * (uint(cell.y) + dims.y * uint(cell.z));
    uint off = lightGridData[idx * 2u], cnt = lightGridData[idx * 2u + 1u];
    for (uint i = 0u; i < cnt; ++i) {
        Light l = lights[lightGridData[off + i]];
        vec3 d = l.posRange.xyz - p;
        float dist2 = max(dot(d, d), 1e-4);
        float r = l.posRange.w;
        float q = dist2 / (r * r);
        float win = clamp(1.0 - q * q, 0.0, 1.0);
        vec3 L = d * inversesqrt(dist2);
        vec3 E = l.color.rgb * (win * win / dist2);
        float NoL = dot(n, L);
        diffuse += E * ((1.0 - translucency) * max(NoL, 0.0) + translucency * max(-NoL, 0.0));
        if (NoL > 0.0) {
            vec3 h = normalize(v + L);
            float NoV = max(dot(n, v), 1e-4);
            spec += E * NoL * D_GGX(max(dot(n, h), 0.0), a) * V_Smith(NoV, NoL, a) * F_Schlick(f0, max(dot(v, h), 0.0));
        }
    }
}

// aerial perspective: the same two-tone quadratic mist as the Blender grade
vec3 applyHaze(vec3 col, vec3 p) {
    vec3 d = p - frame.cameraPos.xyz;
    float dist = length(d);
    float mist = clamp((dist - frame.fogFar.w) / frame.fogParams.x, 0.0, 1.0);
    mist *= mist;
    vec3 hazeCol = mix(frame.fogNear.rgb, frame.fogFar.rgb, pow(mist, 1.5));
    float sunAmt = pow(max(dot(d / max(dist, 1e-3), frame.sunDir.xyz), 0.0), 8.0);
    hazeCol += frame.sunColor.rgb * sunAmt * 0.25 * frame.fogNear.a;
    float f = mist * frame.fogNear.a;
    // low height fog (valley mist)
    if (frame.fogParams.y > 0.0) {
        float hd = frame.fogParams.y * exp(-frame.fogParams.z * max(p.z - frame.fogParams.w, 0.0));
        f = 1.0 - (1.0 - f) * exp(-hd * dist);
    }
    return mix(col, hazeCol, clamp(f, 0.0, 1.0));
}

// bump mapping from a scalar height, using screen-space derivatives
vec3 perturbNormal(vec3 n, vec3 p, float h, float strength) {
    vec3 dpdx = dFdx(p), dpdy = dFdy(p);
    float dhdx = dFdx(h), dhdy = dFdy(h);
    vec3 r1 = cross(dpdy, n), r2 = cross(n, dpdx);
    float det = dot(dpdx, r1);
    if (abs(det) < 1e-9) return n;
    vec3 grad = sign(det) * (dhdx * r1 + dhdy * r2);
    return normalize(abs(det) * n - strength * grad);
}
