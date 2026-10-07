#version 460
// HUD overlay: solid shapes, bitmap-font text (atlas R = glyph, G = outline),
// the minimap texture, anti-aliased discs and rings. Alpha-blended.
layout(set = 0, binding = 1) uniform sampler2D fontAtlas;
layout(set = 0, binding = 2) uniform sampler2D minimap;
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec2 vLocal;
layout(location = 2) in vec4 vColor;
layout(location = 3) flat in uint vMode;
layout(location = 0) out vec4 outColor;

// modes: 0 solid, 1 text, 2 text outline, 3 minimap, 4 round minimap, 5 disc, 6 ring (inner radius in uv.x)
void main() {
    float d = length(vLocal);
    float w = max(fwidth(d), 1e-4);
    float disc = clamp((1.0 - d) / w + 0.5, 0.0, 1.0);
    vec4 c = vColor;
    if (vMode == 1u) c.a *= texture(fontAtlas, vUv).r;
    else if (vMode == 2u) c.a *= texture(fontAtlas, vUv).g;
    else if (vMode == 3u || vMode == 4u) {
        bool inside = all(greaterThanEqual(vUv, vec2(0.0))) && all(lessThanEqual(vUv, vec2(1.0)));
        c.rgb *= inside ? texture(minimap, vUv).rgb : vec3(0.012, 0.014, 0.018);   // beyond the map edge
        if (vMode == 4u) c.a *= disc;
    } else if (vMode == 5u) c.a *= disc;
    else if (vMode == 6u) c.a *= disc * clamp((d - vUv.x) / w + 0.5, 0.0, 1.0);
    if (c.a < 0.002) discard;
    outColor = c;
}
