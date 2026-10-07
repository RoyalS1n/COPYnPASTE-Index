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

// The Blender worlds' cloud plane, ported: two noise octaves with a threshold, self-shadowing from the density a
// few hundred metres towards the sun, lit by translucency (sunlight from above / pi, plus the sky), and hazed like
// distant geometry, because the Blender grade hazes the cloud plane but not the sky behind it. That is what makes
// the reference clouds soft, warm streaks instead of white puffs.
float cloudDensity(vec2 q, float cov) {
    vec3 v = vec3(q, 0.0);
    float n = fbm(v, 6) + (fbm(v * 5.0 + 3.1, 4) - 0.5) * 0.45;
    return pow(clamp((n - (0.66 - cov * 0.28)) / 0.08, 0.0, 1.0), 1.3);
}

// returns rgb premultiplied by coverage, a = coverage
vec4 cloudLayer(vec3 ro, vec3 dir) {
    if (frame.clouds.w < 0.5 || dir.z <= 0.01) return vec4(0.0);
    float t = (frame.clouds.y - ro.z) / dir.z;
    if (t <= 0.0) return vec4(0.0);
    vec3 p = ro + dir * t;
    float sc = frame.clouds.z, cov = frame.clouds.x;
    vec2 m = vec2(p.x, (p.y - 6000.0) * 1.8);   // Blender mapping: plane at y 6000, scale (1, 1.8)
    float d = cloudDensity(m * sc, cov);
    float fade = smoothrange(dir.z, 0.01, 0.12);   // thin out towards the horizon
    if (d * fade <= 0.0) return vec4(0.0);
    vec2 toSun = normalize(frame.sunDir.xy + 1e-4) * 420.0;
    float d2 = cloudDensity((m + toSun) * sc, cov);
    float lit = mix(0.2, 1.0, clamp(1.0 - (d2 - d) * 2.4, 0.0, 1.0));
    vec3 col = frame.cloudColor.rgb * mix(vec3(0.42, 0.46, 0.6), vec3(1.0), lit);
    vec3 light = frame.sunColor.rgb * frame.sunDir.w * max(frame.sunDir.z, 0.0) / PI + skyRadiance(vec3(0.0, 0.0, 1.0));
    vec3 L = col * light;
    float mist = clamp((t - frame.fogFar.w) / frame.fogParams.x, 0.0, 1.0);
    mist *= mist;
    L = mix(L, mix(frame.fogNear.rgb, frame.fogFar.rgb, pow(mist, 1.5)), mist * frame.fogNear.a);
    float a = d * fade;
    return vec4(L * a, a);
}
