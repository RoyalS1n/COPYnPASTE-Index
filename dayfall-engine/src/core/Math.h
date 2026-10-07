#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

// Engine world space: right-handed, Z up, metres (the same as Blender and the
// worldgen pipeline). glTF data (Y up) is converted on load.
namespace df {
using glm::vec2; using glm::vec3; using glm::vec4; using glm::mat3; using glm::mat4; using glm::quat;
using glm::uvec2; using glm::uvec3; using glm::uvec4; using glm::ivec2; using glm::ivec3; using glm::ivec4;

inline vec3 gltfToWorld(const vec3& p) { return {p.x, -p.z, p.y}; }
inline quat gltfToWorld(const quat& q) {          // rotate the frame: Y up -> Z up
    static const quat c = glm::angleAxis(glm::radians(90.0f), vec3(1, 0, 0));
    return c * q * glm::inverse(c);
}
}  // namespace df
