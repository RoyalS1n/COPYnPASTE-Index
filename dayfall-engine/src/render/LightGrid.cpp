#include "render/LightGrid.h"
#include "core/Log.h"
#include <algorithm>
#include <cmath>

namespace df {
LightGrid buildLightGrid(const std::vector<LightDef>& lights) {
    LightGrid g;
    for (auto& l : lights) g.lights.push_back({vec4(l.position, l.range), vec4(l.color * l.intensity, 0)});
    if (lights.empty()) { g.data = {0, 0}; return g; }
    vec3 lo(1e30f), hi(-1e30f);
    for (auto& l : lights) { lo = glm::min(lo, l.position - l.range); hi = glm::max(hi, l.position + l.range); }
    vec3 size = hi - lo;
    g.cell = std::max(6.0f, std::cbrt(size.x * size.y * size.z / 262144.0f));   // at most ~64^3 cells
    g.origin = lo;
    uvec3 d = glm::max(uvec3(glm::ceil(size / g.cell)), uvec3(1));
    g.dims = uvec4(d, (uint32_t)lights.size());
    size_t cells = (size_t)d.x * d.y * d.z;
    std::vector<std::vector<uint32_t>> lists(cells);
    for (uint32_t i = 0; i < lights.size(); ++i) {
        ivec3 a = glm::max(ivec3(glm::floor((lights[i].position - lights[i].range - lo) / g.cell)), ivec3(0));
        ivec3 b = glm::min(ivec3(glm::floor((lights[i].position + lights[i].range - lo) / g.cell)), ivec3(d) - 1);
        for (int z = a.z; z <= b.z; ++z)
            for (int y = a.y; y <= b.y; ++y)
                for (int x = a.x; x <= b.x; ++x) lists[x + d.x * (y + d.y * (size_t)z)].push_back(i);
    }
    g.data.resize(cells * 2);
    for (size_t c = 0; c < cells; ++c) {
        g.data[c * 2] = (uint32_t)g.data.size();
        g.data[c * 2 + 1] = (uint32_t)lists[c].size();
        g.data.insert(g.data.end(), lists[c].begin(), lists[c].end());
    }
    logInfo("light grid: {} lights, {}x{}x{} cells of {:.1f} m", lights.size(), d.x, d.y, d.z, g.cell);
    return g;
}
}  // namespace df
