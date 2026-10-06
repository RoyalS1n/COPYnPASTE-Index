// Sky radiance from the precomputed sky-view LUT (azimuth relative to the
// sun x elevation, more texels near the horizon), plus a cloud layer.
vec2 skyLutUv(vec3 dir) {
    vec3 sd = frame.sunDir.xyz;
    float el = asin(clamp(dir.z, -1.0, 1.0));
    float az = atan(dir.y, dir.x + 1e-7) - atan(sd.y, sd.x + 1e-7);   // atan(0, 0) is undefined
    az = az - 2.0 * PI * floor(az / (2.0 * PI));
    float v = 0.5 + 0.5 * sign(el) * sqrt(abs(el) / (0.5 * PI));
    return vec2(az / (2.0 * PI), v);
}
vec3 skyRadiance(vec3 dir) { return textureLod(skyLut, skyLutUv(dir), 0.0).rgb; }

// returns rgb premultiplied by coverage, a = coverage
vec4 cloudLayer(vec3 ro, vec3 dir) {
    if (frame.clouds.w < 0.5 || dir.z <= 0.01) return vec4(0.0);
    float h = frame.clouds.y;
    float t = (h - ro.z) / dir.z;
    if (t <= 0.0) return vec4(0.0);
    vec3 p = ro + dir * t;
    float sc = frame.clouds.z;
    vec3 q = vec3(p.x * sc, p.y * sc * 0.55, 0.0);
    float cov = frame.clouds.x;
    float n = fbm(q, 6) + (fbm(q * 5.0 + 3.1, 4) - 0.5) * 0.45;
    float d = smoothrange(n, 0.66 - cov * 0.28, 0.74 - cov * 0.28);
    // self-shadowing: density a few hundred metres towards the sun
    vec2 toSun = normalize(frame.sunDir.xy + 1e-4) * 420.0 * sc;
    float n2 = fbm(q + vec3(toSun, 0.0), 6) + (fbm((q + vec3(toSun, 0.0)) * 5.0 + 3.1, 4) - 0.5) * 0.45;
    float d2 = smoothrange(n2, 0.66 - cov * 0.28, 0.74 - cov * 0.28);
    float lit = mix(1.0, 0.2, clamp((d2 - d) * 2.4 * frame.cloudColor.w, 0.0, 1.0));
    vec3 sky = skyRadiance(normalize(vec3(dir.xy, 0.15)));
    vec3 col = frame.cloudColor.rgb * mix(vec3(0.42, 0.46, 0.6), vec3(1.0), lit) * (0.35 * frame.sunDir.w + 0.6 * length(sky));
    float fade = smoothrange(dir.z, 0.01, 0.12);          // thin out towards the horizon
    float a = d * fade;
    return vec4(col * a, a);
}
