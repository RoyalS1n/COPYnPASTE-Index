#include "core/JsonFormat.h"
#include <cmath>
#include <format>

namespace df {
using json = nlohmann::json;
namespace {
std::string scalar(const json& j) {
    if (j.is_number_float()) {
        double v = j.get<double>();
        if (std::isfinite(v) && v == std::round(v) && std::abs(v) < 1e15) return std::format("{:.1f}", v);
        std::string s = std::format("{:.6g}", v);   // drop float noise like 0.10000000149
        return s;
    }
    return j.dump();
}
std::string flat(const json& j) {
    if (j.is_array()) {
        std::string s = "[";
        for (size_t i = 0; i < j.size(); ++i) s += (i ? ", " : "") + flat(j[i]);
        return s + "]";
    }
    if (j.is_object()) {
        std::string s = "{";
        bool first = true;
        for (auto& [k, v] : j.items()) { s += (first ? "" : ", ") + json(k).dump() + ": " + flat(v); first = false; }
        return s + "}";
    }
    return scalar(j);
}
void write(const json& j, int depth, int indent, size_t width, std::string& out) {
    std::string pad((size_t)depth * indent, ' '), pad1((size_t)(depth + 1) * indent, ' ');
    if (j.is_object() || j.is_array()) {
        std::string f = flat(j);
        if (f.size() + pad.size() <= width) { out += f; return; }
    }
    if (j.is_object()) {
        out += "{\n";
        size_t i = 0;
        for (auto& [k, v] : j.items()) {
            out += pad1 + json(k).dump() + ": ";
            write(v, depth + 1, indent, width, out);
            out += ++i < j.size() ? ",\n" : "\n";
        }
        out += pad + "}";
    } else if (j.is_array()) {
        out += "[\n";
        // points and short scalars: several per line
        bool small = true;
        for (auto& e : j) if (flat(e).size() > 24) { small = false; break; }
        if (small) {
            std::string line = pad1;
            for (size_t i = 0; i < j.size(); ++i) {
                std::string e = flat(j[i]) + (i + 1 < j.size() ? "," : "");
                if (line.size() + e.size() + 1 > width && line.size() > pad1.size()) { out += line + "\n"; line = pad1; }
                line += (line.size() > pad1.size() ? " " : "") + e;
            }
            out += line + "\n";
        } else {
            for (size_t i = 0; i < j.size(); ++i) {
                out += pad1;
                write(j[i], depth + 1, indent, width, out);
                out += i + 1 < j.size() ? ",\n" : "\n";
            }
        }
        out += pad + "]";
    } else {
        out += scalar(j);
    }
}
}  // namespace

std::string dumpReadable(const json& j, int indent, size_t width) {
    std::string out;
    write(j, 0, indent, width, out);
    return out;
}
}  // namespace df
