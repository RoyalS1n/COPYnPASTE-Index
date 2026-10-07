#include "world/EnvironmentDoc.h"
#include "core/Error.h"
#include <cmath>

namespace df {
using json = nlohmann::json;
namespace {
vec3 v3(const json& j, vec3 def) { return j.is_array() && j.size() >= 3 ? vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>()) : def; }
float f(const json& j, const char* k, float def) { return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<float>() : def; }

void apply(const json& e, Environment& env) {
    if (e.contains("time_of_day")) {
        // simple solar path: rises in the east at 6:00, highest in the south at 12:00, sets in the west at 18:00
        float t = e["time_of_day"].get<float>();
        float maxEl = f(e, "sun_max_elevation_deg", 55.0f);
        float el = maxEl * std::sin(3.14159265f * (t - 6.0f) / 12.0f);
        float az = 90.0f + (t - 6.0f) * 15.0f;
        env.sunDir = sunDirection(std::max(el, -10.0f), az);
    }
    if (e.contains("sun")) {
        const json& s = e["sun"];
        if (s.contains("direction")) env.sunDir = glm::normalize(v3(s["direction"], env.sunDir));
        else if (s.contains("elevation_deg") || s.contains("azimuth_deg")) {
            float el = glm::degrees(std::asin(glm::clamp(env.sunDir.z, -1.0f, 1.0f)));
            float az = glm::degrees(std::atan2(env.sunDir.x, env.sunDir.y));
            env.sunDir = sunDirection(f(s, "elevation_deg", el), f(s, "azimuth_deg", az));
        }
        env.sunColor = v3(s.value("color", json()), env.sunColor);
        env.sunStrength = f(s, "strength", env.sunStrength);
        env.sunAngleDeg = f(s, "angle_deg", env.sunAngleDeg);
    }
    if (e.contains("sky")) {
        const json& s = e["sky"];
        env.skyStrength = f(s, "strength", env.skyStrength);
        env.airDensity = f(s, "air_density", env.airDensity);
        env.aerosolDensity = f(s, "aerosol_density", env.aerosolDensity);
        env.ozoneDensity = f(s, "ozone_density", env.ozoneDensity);
        env.groundAlbedo = f(s, "ground_albedo", env.groundAlbedo);
        if (s.contains("model")) {
            std::string m = s["model"].get<std::string>();
            if (m != "multiple" && m != "single") throw Error("sky.model must be multiple (default) or single");
            env.skyMultipleScattering = m == "multiple";
        }
    }
    if (e.contains("haze")) {
        const json& h = e["haze"];
        env.hazeNear = v3(h.value("color_near", json()), env.hazeNear);
        env.hazeFar = v3(h.value("color_far", json()), env.hazeFar);
        env.hazeAmount = f(h, "amount", env.hazeAmount);
        env.mistStart = f(h, "start_m", env.mistStart);
        env.mistDepth = f(h, "depth_m", env.mistDepth);
        env.heightFogDensity = f(h, "height_fog_density", env.heightFogDensity);
        env.heightFogFalloff = f(h, "height_fog_falloff", env.heightFogFalloff);
        env.heightFogBase = f(h, "height_fog_base_m", env.heightFogBase);
    }
    if (e.contains("clouds")) {
        const json& c = e["clouds"];
        env.clouds = c.value("enabled", env.clouds);
        env.cloudCoverage = f(c, "coverage", env.cloudCoverage);
        env.cloudHeight = f(c, "height_m", env.cloudHeight);
        env.cloudScale = f(c, "scale", env.cloudScale);
        env.cloudColor = v3(c.value("color", json()), env.cloudColor);
    }
    if (e.contains("tonemap")) {
        const json& t = e["tonemap"];
        env.exposureEv = f(t, "exposure_ev", env.exposureEv);
        env.lookPower = f(t, "contrast", env.lookPower);
        env.lookSaturation = f(t, "saturation", env.lookSaturation);
        env.vignette = f(t, "vignette", env.vignette);
    }
    if (e.contains("wind")) {
        const json& w = e["wind"];
        if (w.contains("direction")) {
            vec2 d(w["direction"][0].get<float>(), w["direction"][1].get<float>());
            if (glm::length(d) > 1e-4f) env.windDir = glm::normalize(d);
        }
        env.windStrength = f(w, "strength", env.windStrength);
    }
    if (e.contains("water")) {
        const json& w = e["water"];
        env.waterLevel = w.value("enabled", false) ? f(w, "level_m", 0.0f) : -1000.0f;
    }
    env.shadowDistance = f(e, "shadow_distance_m", env.shadowDistance);
    if (e.contains("sky_occlusion")) {
        const json& o = e["sky_occlusion"];
        if (o.is_boolean()) env.skyOcclusion = o.get<bool>();
        else if (o.is_object()) {
            env.skyOcclusion = o.value("enabled", env.skyOcclusion);
            env.skyOccCell = f(o, "cell_m", env.skyOccCell);
            env.skyOccRays = (uint32_t)f(o, "rays", (float)env.skyOccRays);
            env.skyOccStrength = f(o, "strength", env.skyOccStrength);
            if (!(env.skyOccCell >= 0.25f && env.skyOccCell <= 4.0f)) throw Error("sky_occlusion.cell_m must be 0.25 to 4");
            if (env.skyOccRays < 8 || env.skyOccRays > 256) throw Error("sky_occlusion.rays must be 8 to 256");
            if (!(env.skyOccStrength >= 0.0f && env.skyOccStrength <= 1.0f)) throw Error("sky_occlusion.strength must be 0 to 1");
        } else throw Error("sky_occlusion must be true, false or {enabled, cell_m, rays, strength}");
    }
}
}  // namespace

vec3 sunDirection(float elevationDeg, float azimuthDeg) {
    float el = glm::radians(elevationDeg), az = glm::radians(azimuthDeg);
    return glm::normalize(vec3(std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)));
}

const json& environmentPresets() {
    // tuned against the reference-world Blender renders (golden_valley, misty_morning, alpine_dusk, serene_meadow)
    static const json p = json::parse(R"({
      "golden_hour": {"sun": {"elevation_deg": 14, "azimuth_deg": -163, "strength": 5.5, "color": [1.0, 0.7, 0.42], "angle_deg": 0.6},
                      "sky": {"strength": 0.1, "aerosol_density": 1.6},
                      "haze": {"color_near": [0.62, 0.52, 0.42], "color_far": [0.48, 0.47, 0.5], "amount": 0.72, "start_m": 30, "depth_m": 6500},
                      "clouds": {"enabled": true, "coverage": 0.55, "color": [1.0, 0.7, 0.5]},
                      "tonemap": {"exposure_ev": 0.9, "contrast": 1.15, "saturation": 1.05, "vignette": 0.25}},
      "serene":      {"sun": {"elevation_deg": 16, "azimuth_deg": -128, "strength": 5.0, "color": [1.0, 0.76, 0.52], "angle_deg": 0.6},
                      "sky": {"strength": 0.1, "aerosol_density": 1.6},
                      "haze": {"color_near": [0.62, 0.54, 0.45], "color_far": [0.5, 0.5, 0.53], "amount": 0.6, "start_m": 30, "depth_m": 6500},
                      "clouds": {"enabled": true, "coverage": 0.5, "color": [1.0, 0.78, 0.6]},
                      "tonemap": {"exposure_ev": 0.85, "contrast": 1.12, "saturation": 1.05, "vignette": 0.22}},
      "noon":        {"sun": {"elevation_deg": 58, "azimuth_deg": 170, "strength": 6.5, "color": [1.0, 0.97, 0.92], "angle_deg": 0.55},
                      "sky": {"strength": 0.12, "aerosol_density": 1.0},
                      "haze": {"color_near": [0.6, 0.66, 0.75], "color_far": [0.62, 0.7, 0.82], "amount": 0.45, "start_m": 60, "depth_m": 9000},
                      "clouds": {"enabled": true, "coverage": 0.4, "color": [1.0, 1.0, 1.0]},
                      "tonemap": {"exposure_ev": 0.0, "contrast": 1.1, "saturation": 1.05, "vignette": 0.18}},
      "misty_morning": {"sun": {"elevation_deg": 18, "azimuth_deg": 168, "strength": 3.0, "color": [1.0, 0.88, 0.74], "angle_deg": 3.0},
                      "sky": {"strength": 0.14, "aerosol_density": 5.0},
                      "haze": {"color_near": [0.52, 0.57, 0.62], "color_far": [0.55, 0.6, 0.66], "amount": 0.9, "start_m": 5, "depth_m": 2400,
                               "height_fog_density": 0.012, "height_fog_falloff": 0.12, "height_fog_base_m": 0},
                      "clouds": {"enabled": true, "coverage": 0.6, "color": [0.95, 0.95, 0.97]},
                      "tonemap": {"exposure_ev": 0.7, "contrast": 1.0, "saturation": 0.95, "vignette": 0.2}},
      "dusk":        {"sun": {"elevation_deg": 3, "azimuth_deg": -95, "strength": 4.0, "color": [1.0, 0.5, 0.3], "angle_deg": 0.6},
                      "sky": {"strength": 0.08, "aerosol_density": 2.0},
                      "haze": {"color_near": [0.5, 0.38, 0.45], "color_far": [0.42, 0.36, 0.48], "amount": 0.65, "start_m": 30, "depth_m": 5000},
                      "clouds": {"enabled": true, "coverage": 0.5, "color": [1.0, 0.5, 0.4]},
                      "tonemap": {"exposure_ev": 1.2, "contrast": 1.35, "saturation": 1.3, "vignette": 0.3}},
      "overcast":    {"sun": {"elevation_deg": 35, "azimuth_deg": 150, "strength": 0.8, "color": [0.9, 0.92, 0.95], "angle_deg": 12},
                      "sky": {"strength": 0.2, "aerosol_density": 6.0},
                      "haze": {"color_near": [0.55, 0.57, 0.6], "color_far": [0.6, 0.62, 0.65], "amount": 0.75, "start_m": 20, "depth_m": 3500},
                      "clouds": {"enabled": true, "coverage": 0.95, "color": [0.8, 0.82, 0.85]},
                      "tonemap": {"exposure_ev": 1.3, "contrast": 1.0, "saturation": 0.9, "vignette": 0.2}}
    })");
    return p;
}

void parseEnvironment(const json& e, Environment& env) {
    env = Environment();
    if (!e.is_object()) return;
    if (e.contains("preset")) {
        std::string name = e["preset"].get<std::string>();
        const json& p = environmentPresets();
        if (!p.contains(name)) throw Error("unknown environment preset '" + name + "' (golden_hour, serene, noon, misty_morning, dusk, overcast)");
        apply(p[name], env);
    }
    apply(e, env);
}
}  // namespace df
