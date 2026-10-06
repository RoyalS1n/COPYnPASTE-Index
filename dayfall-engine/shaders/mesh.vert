#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "vertex.glsl"
layout(location = 0) out vec3 vPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec4 vUv;
layout(location = 4) out vec4 vColor0;
layout(location = 5) out vec4 vColor1;
layout(location = 6) flat out uint vMaterial;
layout(location = 7) flat out vec4 vInst;

void main() {
    Pulled p = pullVertex();
    vPos = p.pos;
    vNormal = p.normal;
    vTangent = p.tangent;
    vUv = p.uv;
    vColor0 = p.color0;
    vColor1 = p.color1;
    vMaterial = p.material;
    vInst = p.inst;
    gl_Position = frame.viewProj * vec4(p.pos, 1.0);
}
