// Vertex pulling shared by the mesh and shadow vertex shaders.
layout(std430, set = 0, binding = 8) readonly buffer Visible { uint visible[]; };

struct Pulled {
    vec3 pos;          // world
    vec3 normal;       // world
    vec4 tangent;      // world xyz, handedness
    vec4 uv;           // uv0, uv1
    vec4 color0, color1;
    uint material;
    vec4 inst;         // instance origin, seed
};

Pulled pullVertex() {
    uint batch = pc.batchBase + gl_DrawID;
    Batch b = batches[batch];
    uint id = visible[gl_InstanceIndex];
    Instance inst = instances[id];
    Vertex v = vertices[gl_VertexIndex];
    Material mat = materials[b.material];
    vec3 s = instanceScale(inst);
    vec3 local = v.p0.xyz;
    Pulled o;
    if ((mat.h0.y & MAT_WIND) != 0u) {
        // sway grows with height above the mesh origin; phase varies per instance
        float h = clamp(local.z / max(mat.p[1].y, 0.01), 0.0, 1.0);
        float t = frame.cameraPos.w * mat.p[1].z;
        float ph = inst.seed * 6.2831 + dot(inst.posScale.xy, vec2(0.13, 0.17));
        float sw = sin(t + ph) * 0.7 + sin(t * 2.31 + ph * 1.7) * 0.3;
        vec2 w = frame.wind.xy * (sw * mat.p[1].x * frame.wind.z * h * h);
        local.xy += w / max(s.xy, vec2(1e-3));
    }
    o.pos = inst.posScale.xyz + quatRotate(inst.rot, local * s);
    vec3 n = v.p1.xyz, t = v.tangent.xyz;
    if ((inst.flags & INST_AXIS_SCALE) != 0u) {   // stretched: normals by the inverse scale, tangents by the scale
        n = normalize(n / s);
        t = normalize(t * s);
    }
    o.normal = quatRotate(inst.rot, n);
    o.tangent = vec4(quatRotate(inst.rot, t), v.tangent.w);
    o.uv = vec4(v.p0.w, v.p1.w, v.p3.x, v.p3.y);
    o.color0 = unpackColor(v.p3.z);
    o.color1 = unpackColor(v.p3.w);
    o.material = b.material;
    o.inst = vec4(inst.posScale.xyz, inst.seed);
    return o;
}
