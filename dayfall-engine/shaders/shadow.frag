#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 1) flat in uint vMaterial;

void main() {
    Material m = materials[vMaterial];
    if (m.h0.z != 0xFFFFFFFFu) {
        float a = texture(textures[nonuniformEXT(m.h0.z)], vUv).a * m.c[0].a;
        if (a < uintBitsToFloat(m.h1.z)) discard;
    }
}
