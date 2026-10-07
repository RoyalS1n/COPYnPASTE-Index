#version 460
// HUD overlay: screen-space vertices pulled from the HUD's own buffer (pixels, y down).
struct HudVertex { vec2 pos; vec2 uv; vec2 local; uint color; uint mode; };
layout(std430, set = 0, binding = 0) readonly buffer Verts { HudVertex verts[]; };
layout(push_constant) uniform Push { vec2 invSize; vec2 pad; } pc;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec2 vLocal;
layout(location = 2) out vec4 vColor;
layout(location = 3) flat out uint vMode;

void main() {
    HudVertex v = verts[gl_VertexIndex];
    vec4 c = unpackUnorm4x8(v.color);
    vColor = vec4(pow(c.rgb, vec3(2.2)), c.a);   // colours are given in sRGB; blending happens in linear
    vUv = v.uv;
    vLocal = v.local;
    vMode = v.mode;
    gl_Position = vec4(v.pos * pc.invSize * 2.0 - 1.0, 0.0, 1.0);
}
