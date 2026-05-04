#pragma once
// Shared scene/mesh types visible to both engine and editor.
// Keep this header free of platform-specific includes.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace psx {

// ── Compact vertex (matches GPU layout) ─────────────────────────────────────
struct Vertex {
    float x, y, z;      // position
    float nx, ny, nz;   // normal
    float u, v;         // UVs (0..1, will be snapped in shader)
    uint8_t r, g, b, a; // vertex colour
};

// ── Scene object ─────────────────────────────────────────────────────────────
struct SceneObject {
    std::string name;
    std::string mesh_path;    // relative to assets/
    std::string texture_path; // relative to assets/, empty = untextured
    std::array<float,3> position   = {0,0,0};
    std::array<float,3> rotation   = {0,0,0}; // Euler XYZ degrees
    std::array<float,3> scale      = {1,1,1};
};

// ── Scene ─────────────────────────────────────────────────────────────────────
struct Scene {
    std::string name    = "untitled";
    std::vector<SceneObject> objects;
};

} // namespace psx
