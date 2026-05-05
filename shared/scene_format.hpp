#pragma once
// psx::Node / psx::Scene — v2 tree-based scene format.
// Platform-free: only GLM + nlohmann_json + standard library.
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

namespace psx {

// ── Node ─────────────────────────────────────────────────────────────────────
struct Node {
    int              id       = 0;
    std::string      name;
    std::string      kind     = "node"; // "node"|"mesh"|"camera"|"light"|"player"
    int              parent   = -1;
    std::vector<int> children;
    glm::vec3        position = {0.f, 0.f, 0.f};
    // Quaternion (w, x, y, z) — identity. Avoids Euler gimbal lock.
    glm::quat        rotation = glm::quat(1.f, 0.f, 0.f, 0.f);
    glm::vec3        scale    = {1.f, 1.f, 1.f};
    bool             visible  = true;
    nlohmann::json   components = nlohmann::json::object(); // {"mesh":{…}, …}
};

// ── Scene ────────────────────────────────────────────────────────────────────
struct Scene {
    int               format_version = 2;
    std::string       name           = "untitled";
    std::vector<Node> nodes;
    int               next_id        = 0;

    // Assigns id, appends to nodes, wires parent→children. Returns new id.
    int   add_node(Node n);

    Node*       find(int id);
    const Node* find(int id) const;

    // Accumulated world transform (local TRS walk up to root).
    glm::mat4 world_transform(int id) const;

    // Migrates v1 { "objects":[…] } JSON document to v2 Scene.
    static Scene from_v1(const nlohmann::json& v1_doc);
};

// ── to_json / from_json — Node ────────────────────────────────────────────────
inline void to_json(nlohmann::json& j, const Node& n) {
    j = nlohmann::json{
        {"id",         n.id},
        {"name",       n.name},
        {"kind",       n.kind},
        {"parent",     n.parent},
        {"children",   n.children},
        {"position",   {n.position.x, n.position.y, n.position.z}},
        // Quaternion serialised as [x, y, z, w].
        {"rotation",   {n.rotation.x, n.rotation.y, n.rotation.z, n.rotation.w}},
        {"scale",      {n.scale.x,    n.scale.y,    n.scale.z}},
        {"visible",    n.visible},
        {"components", n.components},
    };
}

inline void from_json(const nlohmann::json& j, Node& n) {
    n.id         = j.value("id",       0);
    n.name       = j.value("name",     "");
    n.kind       = j.value("kind",     "node");
    n.parent     = j.value("parent",   -1);
    n.children   = j.value("children", std::vector<int>{});
    n.visible    = j.value("visible",  true);
    n.components = j.value("components", nlohmann::json::object());
    auto get3 = [&](const char* key, glm::vec3 def) -> glm::vec3 {
        auto v = j.value(key, std::vector<float>{def.x, def.y, def.z});
        return (v.size() == 3) ? glm::vec3{v[0], v[1], v[2]} : def;
    };
    n.position = get3("position", {0.f, 0.f, 0.f});
    n.scale    = get3("scale",    {1.f, 1.f, 1.f});
    // Rotation: 4-element [x, y, z, w] quaternion (v2 format).
    {
        auto v = j.value("rotation",
                         std::vector<float>{0.f, 0.f, 0.f, 1.f});
        if (v.size() == 4) n.rotation = glm::quat(v[3], v[0], v[1], v[2]);
        else               n.rotation = glm::quat(1.f, 0.f, 0.f, 0.f);
    }
}

// ── to_json / from_json — Scene ───────────────────────────────────────────────
inline void to_json(nlohmann::json& j, const Scene& s) {
    j = nlohmann::json{
        {"format_version", s.format_version},
        {"name",           s.name},
        {"next_id",        s.next_id},
        {"nodes",          s.nodes},
    };
}

inline void from_json(const nlohmann::json& j, Scene& s) {
    s.format_version = j.value("format_version", 2);
    s.name           = j.value("name",    "untitled");
    s.next_id        = j.value("next_id", 0);
    s.nodes          = j.value("nodes",   std::vector<Node>{});
}

// ── Scene method implementations ──────────────────────────────────────────────
inline int Scene::add_node(Node n) {
    const int id = next_id++;
    n.id = id;
    // Wire into parent before push_back (pointer stays valid until push_back).
    if (n.parent != -1) {
        Node* p = find(n.parent);
        if (p) p->children.push_back(id);
    }
    nodes.push_back(std::move(n));
    return id;
}

inline Node* Scene::find(int id) {
    for (auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

inline const Node* Scene::find(int id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

inline glm::mat4 Scene::world_transform(int id) const {
    const Node* n = find(id);
    if (!n) return glm::mat4(1.f);
    glm::mat4 local = glm::translate(glm::mat4(1.f), n->position);
    local = local * glm::mat4_cast(n->rotation);
    local = glm::scale(local, n->scale);
    if (n->parent == -1) return local;
    return world_transform(n->parent) * local;
}

inline Scene Scene::from_v1(const nlohmann::json& v1_doc) {
    Scene s;
    s.name = v1_doc.value("name", "untitled");

    // Root grouping node
    Node root;
    root.name   = "Level";
    root.kind   = "node";
    root.parent = -1;
    const int root_id = s.add_node(std::move(root));

    for (const auto& o : v1_doc.value("objects", nlohmann::json::array())) {
        Node n;
        n.name    = o.value("name",    "Object");
        n.kind    = "mesh";
        n.parent  = root_id;
        n.visible = o.value("visible", true);

        auto get3 = [&](const char* key, glm::vec3 def) -> glm::vec3 {
            auto v = o.value(key, std::vector<float>{def.x, def.y, def.z});
            return (v.size() == 3) ? glm::vec3{v[0], v[1], v[2]} : def;
        };
        n.position = get3("position", {0.f, 0.f, 0.f});
        // v1 stored Euler XYZ degrees — convert to quat for v2.
        glm::vec3 rot_deg = get3("rotation", {0.f, 0.f, 0.f});
        n.rotation = glm::quat(glm::radians(rot_deg));
        n.scale    = get3("scale",    {1.f, 1.f, 1.f});

        n.components["mesh"]["path"]      = o.value("mesh_path",    "");
        n.components["mesh"]["texture"]   = o.value("texture_path", "");
        n.components["collision"]["type"] = o.value("collision",    "box");

        s.add_node(std::move(n));
    }

    return s;
}

// ── load_scene / save_scene ───────────────────────────────────────────────────
inline Scene load_scene(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open scene: " + path);
    const nlohmann::json doc = nlohmann::json::parse(f);

    // v2: explicit version tag OR "nodes" array present
    if (doc.value("format_version", 0) == 2 || doc.contains("nodes"))
        return doc.get<Scene>();

    // v1: flat "objects" array
    if (doc.contains("objects"))
        return Scene::from_v1(doc);

    throw std::runtime_error("Unknown scene format in: " + path);
}

inline void save_scene(const Scene& s, const std::string& path) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write scene: " + path);
    f << nlohmann::json(s).dump(2);
}

} // namespace psx
