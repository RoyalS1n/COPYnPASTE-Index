// GPU heightmap terrain: sampling helpers shared by terrain.vert / terrain.frag.
vec2 terrainUv(vec2 p) {
    float n = frame.terrainA.w;
    vec2 g = (p - frame.terrainA.xy) / frame.terrainA.z;     // sample coordinates
    return (g + 0.5) / n;
}
float terrainHeightAt(vec2 p) {
    return frame.terrainB.x + textureLod(terrainHeight, terrainUv(p), 0.0).r * frame.terrainB.y;
}
vec3 terrainNormalAt(vec2 p) {
    float e = frame.terrainA.z;
    float hx = terrainHeightAt(p + vec2(e, 0)) - terrainHeightAt(p - vec2(e, 0));
    float hy = terrainHeightAt(p + vec2(0, e)) - terrainHeightAt(p - vec2(0, e));
    return normalize(vec3(-hx, -hy, 2.0 * e));
}
