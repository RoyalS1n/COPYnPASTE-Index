#pragma once
#include "core/Math.h"

namespace df {
struct Camera {
    vec3 position{0, 0, 2};
    float yaw = 0.0f;      // radians, 0 = looking along +Y, increasing to the left (counter-clockwise from above)
    float pitch = 0.0f;    // radians, positive = up
    float vfov = glm::radians(45.0f);
    float nearPlane = 0.1f;

    vec3 forward() const { return {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)}; }
    void lookAt(const vec3& target) {
        vec3 d = glm::normalize(target - position);
        pitch = std::asin(glm::clamp(d.z, -1.0f, 1.0f));
        yaw = std::atan2(-d.x, d.y);
    }
    mat4 view() const { return glm::lookAt(position, position + forward(), vec3(0, 0, 1)); }
    // reversed-Z, infinite far plane: depth = near / distance
    mat4 projection(float aspect) const {
        float f = 1.0f / std::tan(vfov * 0.5f);
        mat4 p(0.0f);
        p[0][0] = f / aspect;
        p[1][1] = f;
        p[2][3] = -1.0f;
        p[3][2] = nearPlane;
        return p;
    }
};
}  // namespace df
