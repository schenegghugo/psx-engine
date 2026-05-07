// psx-editor — milestone 3 + built-in primitive meshes
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cfloat>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <map>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#if defined(_WIN32)
#  include <process.h>          // _getpid
#  define psx_getpid() _getpid()
#else
#  include <unistd.h>           // getpid
#  define psx_getpid() getpid()
#endif

#include <nlohmann/json.hpp>

#include "scene_format.hpp"

#include <SDL2/SDL.h>

#if __has_include(<glad/gl.h>)
#  include <glad/gl.h>
#else
#  include <glad/glad.h>
#endif

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#ifdef HAVE_IMGUIZMO
#  include <ImGuizmo.h>
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

// ── Helpers ───────────────────────────────────────────────────────────────────
static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return ""; }
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

// ── Built-in ImGui file browser ──────────────────────────────────────────────
// Self-contained — no external dependency on zenity / kdialog / yazi / etc.
// Three modes: pick an existing file, save to a (possibly new) file, pick a
// directory. The Browse buttons in the New Project + Save Scene As modals
// open this; other tools can be plugged in by users via the
// $PSX_FILE_PICKER env var (handled at the call site).
struct FsBrowser {
    enum Mode { OPEN_FILE, SAVE_FILE, OPEN_DIR };
    Mode        mode  = OPEN_FILE;
    std::string title;
    std::string cwd;            // absolute path of the currently-displayed dir
    char        filename[256]   = "";
    std::vector<std::string>                exts;       // [".pscene"], empty = any
    std::function<void(const std::string&)> on_accept;  // called on Save/Open
    bool        open_request = false;                   // set to pop the modal
};

static void fs_browser_render(FsBrowser& b) {
    namespace fs = std::filesystem;

    if (b.open_request) {
        ImGui::OpenPopup("##fs_browser");
        b.open_request = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({640.f, 480.f}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##fs_browser", nullptr,
                                ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::TextUnformatted(b.title.c_str());
    ImGui::Separator();

    // Path bar
    if (ImGui::Button("..")) {
        fs::path p(b.cwd);
        if (p.has_parent_path()) b.cwd = p.parent_path().string();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(b.cwd.c_str());
    ImGui::Separator();

    // Build entry list — directories first, then matching files.
    std::error_code ec;
    struct Entry { std::string name; bool is_dir; };
    std::vector<Entry> entries;
    for (const auto& e : fs::directory_iterator(b.cwd, ec)) {
        std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;   // skip hidden
        const bool is_dir = e.is_directory(ec);
        if (!is_dir) {
            if (b.mode == FsBrowser::OPEN_DIR) continue;
            if (!b.exts.empty()) {
                std::string ext = e.path().extension().string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                bool match = false;
                for (const auto& want : b.exts) {
                    std::string lw = want;
                    for (char& c : lw) c = (char)std::tolower((unsigned char)c);
                    if (ext == lw) { match = true; break; }
                }
                if (!match) continue;
            }
        }
        entries.push_back({std::move(name), is_dir});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& bb) {
                  if (a.is_dir != bb.is_dir) return a.is_dir;
                  return a.name < bb.name;
              });

    bool accept = false;
    ImGui::BeginChild("##fs_list", {0, -64}, true);
    for (const auto& e : entries) {
        const std::string label = (e.is_dir ? "[D] " : "    ") + e.name;
        if (ImGui::Selectable(label.c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            if (e.is_dir) {
                if (ImGui::IsMouseDoubleClicked(0))
                    b.cwd = (fs::path(b.cwd) / e.name).string();
            } else {
                std::snprintf(b.filename, sizeof(b.filename),
                              "%s", e.name.c_str());
                if (ImGui::IsMouseDoubleClicked(0)) accept = true;
            }
        }
    }
    ImGui::EndChild();

    if (b.mode != FsBrowser::OPEN_DIR) {
        ImGui::Text("Filename:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##fs_filename", b.filename, sizeof(b.filename));
    }

    const char* accept_label =
        b.mode == FsBrowser::OPEN_DIR  ? "Select" :
        b.mode == FsBrowser::SAVE_FILE ? "Save"   : "Open";
    const bool can_accept =
        (b.mode == FsBrowser::OPEN_DIR) || (b.filename[0] != '\0');
    ImGui::BeginDisabled(!can_accept);
    if (ImGui::Button(accept_label, {120, 0})) accept = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {120, 0})) {
        b.on_accept = nullptr;
        ImGui::CloseCurrentPopup();
    }

    if (accept) {
        std::string out = (b.mode == FsBrowser::OPEN_DIR)
            ? b.cwd
            : (fs::path(b.cwd) / b.filename).string();
        auto cb = std::move(b.on_accept);
        b.on_accept = nullptr;
        ImGui::CloseCurrentPopup();
        if (cb) cb(out);
    }

    ImGui::EndPopup();
}

// Optional escape hatch: if the user has set $PSX_FILE_PICKER, run that
// command instead of opening the built-in browser. The template should
// contain `{out}` somewhere — we replace it with a temp file path and read
// the picked path from that file after the command exits. This blocks the
// main loop until the external picker closes (yazi, fzf-in-a-terminal, etc.).
//
// Example: PSX_FILE_PICKER='foot -e yazi --chooser-file={out}'
static std::string run_external_picker() {
    const char* tmpl_c = std::getenv("PSX_FILE_PICKER");
    if (!tmpl_c || !*tmpl_c) return std::string();

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec)
                 / ("psx_picker_" + std::to_string((long)psx_getpid()) + ".txt");
    // Make sure the file exists and is empty so a clean cancel returns "".
    { std::ofstream(tmp.string()).close(); }

    std::string cmd = tmpl_c;
    const std::string token = "{out}";
    for (size_t pos = cmd.find(token); pos != std::string::npos;
         pos = cmd.find(token, pos))
    {
        // Single-quote-escape the temp path before substituting.
        std::string q = "'";
        for (char c : tmp.string()) q += (c == '\'') ? std::string("'\\''")
                                                     : std::string(1, c);
        q += "'";
        cmd.replace(pos, token.size(), q);
        pos += q.size();
    }

    int rc = std::system(cmd.c_str());
    (void)rc;

    std::string result;
    {
        std::ifstream f(tmp.string());
        std::getline(f, result);
    }
    fs::remove(tmp, ec);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
        fprintf(stderr, "Shader compile error:\n%s\n", log);
    }
    return s;
}

static GLuint link_program(GLuint vert, GLuint frag) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vert); glAttachShader(p, frag);
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, 512, nullptr, log);
        fprintf(stderr, "Shader link error:\n%s\n", log);
    }
    glDeleteShader(vert); glDeleteShader(frag);
    return p;
}

// ── TGA ───────────────────────────────────────────────────────────────────────
static void write_checkerboard_tga(const char* path, int size, int cell) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot write TGA: %s\n", path); return; }
    uint8_t hdr[18] = {};
    hdr[2]  = 2;
    hdr[12] = (uint8_t)(size & 0xFF);  hdr[13] = (uint8_t)(size >> 8);
    hdr[14] = (uint8_t)(size & 0xFF);  hdr[15] = (uint8_t)(size >> 8);
    hdr[16] = 24;  hdr[17] = 0x20;
    fwrite(hdr, 1, 18, f);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            uint8_t c = (((x/cell) ^ (y/cell)) & 1) ? 220 : 32;
            uint8_t px[3] = {c, c, c};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "TGA: wrote %dx%d checkerboard -> %s\n", size, size, path);
}

struct TGAImage { int w = 0, h = 0; std::vector<uint8_t> rgb; };

static TGAImage load_tga(const char* path) {
    TGAImage img;
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open TGA: %s\n", path); return img; }
    uint8_t hdr[18];
    if (fread(hdr, 1, 18, f) != 18) { fclose(f); return img; }
    int id_len = hdr[0], imgtype = hdr[2];
    int w = hdr[12] | (hdr[13] << 8), h = hdr[14] | (hdr[15] << 8);
    int bpp = hdr[16], desc = hdr[17];
    if (imgtype != 2 || bpp != 24) {
        fprintf(stderr, "TGA: unsupported type=%d bpp=%d\n", imgtype, bpp);
        fclose(f); return img;
    }
    fseek(f, id_len, SEEK_CUR);
    img.w = w; img.h = h;
    img.rgb.resize((size_t)(w * h * 3));
    for (int i = 0; i < w * h; ++i) {
        uint8_t bgr[3];
        if (fread(bgr, 1, 3, f) != 3) break;
        img.rgb[i*3+0] = bgr[2]; img.rgb[i*3+1] = bgr[1]; img.rgb[i*3+2] = bgr[0];
    }
    fclose(f);
    if (desc & 0x20) {
        for (int row = 0; row < h/2; ++row) {
            int mirror = h - 1 - row;
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    std::swap(img.rgb[(row*w+x)*3+c], img.rgb[(mirror*w+x)*3+c]);
        }
    }
    fprintf(stderr, "TGA: loaded %dx%d %s\n", w, h, path);
    return img;
}

// ── OBJ loader — stride 8: pos(3) uv(2) colour(3) ────────────────────────────
struct LoadedMesh {
    std::vector<float> verts;
    glm::vec3 bbox_min = { FLT_MAX,  FLT_MAX,  FLT_MAX};
    glm::vec3 bbox_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
};

static LoadedMesh load_obj(const char* path) {
    LoadedMesh result;
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open OBJ: %s\n", path); return result; }

    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> uvs;
    struct FV { int vi, vti; };
    std::vector<std::array<FV,3>> tris;

    static const glm::vec3 PALETTE[] = {
        {1.f,.35f,.35f},{.35f,1.f,.35f},{.35f,.35f,1.f},
        {1.f,1.f,.35f},{1.f,.35f,1.f},{.35f,1.f,1.f},
    };

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line); std::string tok; ss >> tok;
        if (tok == "v") {
            float x,y,z; ss>>x>>y>>z; positions.push_back({x,y,z});
        } else if (tok == "vt") {
            float u,v; ss>>u>>v; uvs.push_back({u,v});
        } else if (tok == "f") {
            std::vector<FV> poly; std::string t;
            while (ss >> t) {
                size_t s1 = t.find('/');
                int vi = std::stoi(t.substr(0, s1));
                if (vi < 0) vi = (int)positions.size() + vi + 1;
                vi -= 1;
                int vti = -1;
                if (s1 != std::string::npos) {
                    size_t s2 = t.find('/', s1+1);
                    std::string vts = t.substr(s1+1, s2 == std::string::npos
                                               ? std::string::npos : s2-s1-1);
                    if (!vts.empty()) {
                        vti = std::stoi(vts);
                        if (vti < 0) vti = (int)uvs.size() + vti + 1;
                        vti -= 1;
                    }
                }
                poly.push_back({vi, vti});
            }
            for (int i = 1; i+1 < (int)poly.size(); ++i)
                tris.push_back({poly[0], poly[i], poly[i+1]});
        }
    }

    for (const auto& p : positions) {
        result.bbox_min = glm::min(result.bbox_min, p);
        result.bbox_max = glm::max(result.bbox_max, p);
    }
    if (positions.empty()) {
        result.bbox_min = {-1.f,-1.f,-1.f};
        result.bbox_max = { 1.f, 1.f, 1.f};
    }

    result.verts.reserve(tris.size() * 3 * 8);
    for (int ti = 0; ti < (int)tris.size(); ++ti) {
        const glm::vec3& col = PALETTE[ti % 6];
        for (int k = 0; k < 3; ++k) {
            const FV& fv = tris[ti][k];
            const glm::vec3& p = positions[fv.vi];
            glm::vec2 uv = (fv.vti >= 0 && fv.vti < (int)uvs.size())
                           ? uvs[fv.vti] : glm::vec2{0.f, 0.f};
            result.verts.insert(result.verts.end(),
                {p.x,p.y,p.z, uv.x,uv.y, col.r,col.g,col.b});
        }
    }
    fprintf(stderr, "OBJ: %d tris (%s)\n", (int)tris.size(), path);
    return result;
}

static LoadedMesh make_unit_cube() {
    LoadedMesh r;
    r.bbox_min = {-1.f,-1.f,-1.f};
    r.bbox_max = { 1.f, 1.f, 1.f};
    static const float d[] = {
        -1,-1, 1, 0,0, 1.f,.35f,.35f,   1,-1, 1, 1,0, 1.f,.35f,.35f,
         1, 1, 1, 1,1, 1.f,.35f,.35f,  -1,-1, 1, 0,0, 1.f,.35f,.35f,
         1, 1, 1, 1,1, 1.f,.35f,.35f,  -1, 1, 1, 0,1, 1.f,.35f,.35f,
         1,-1,-1, 0,0, .35f,1.f,.35f,  -1,-1,-1, 1,0, .35f,1.f,.35f,
        -1, 1,-1, 1,1, .35f,1.f,.35f,   1,-1,-1, 0,0, .35f,1.f,.35f,
        -1, 1,-1, 1,1, .35f,1.f,.35f,   1, 1,-1, 0,1, .35f,1.f,.35f,
        -1,-1,-1, 0,0, .35f,.35f,1.f,  -1,-1, 1, 1,0, .35f,.35f,1.f,
        -1, 1, 1, 1,1, .35f,.35f,1.f,  -1,-1,-1, 0,0, .35f,.35f,1.f,
        -1, 1, 1, 1,1, .35f,.35f,1.f,  -1, 1,-1, 0,1, .35f,.35f,1.f,
         1,-1, 1, 0,0, 1.f,1.f,.35f,   1,-1,-1, 1,0, 1.f,1.f,.35f,
         1, 1,-1, 1,1, 1.f,1.f,.35f,   1,-1, 1, 0,0, 1.f,1.f,.35f,
         1, 1,-1, 1,1, 1.f,1.f,.35f,   1, 1, 1, 0,1, 1.f,1.f,.35f,
        -1, 1, 1, 0,0, 1.f,.35f,1.f,   1, 1, 1, 1,0, 1.f,.35f,1.f,
         1, 1,-1, 1,1, 1.f,.35f,1.f,  -1, 1, 1, 0,0, 1.f,.35f,1.f,
         1, 1,-1, 1,1, 1.f,.35f,1.f,  -1, 1,-1, 0,1, 1.f,.35f,1.f,
        -1,-1,-1, 0,0, .35f,1.f,1.f,   1,-1,-1, 1,0, .35f,1.f,1.f,
         1,-1, 1, 1,1, .35f,1.f,1.f,  -1,-1,-1, 0,0, .35f,1.f,1.f,
         1,-1, 1, 1,1, .35f,1.f,1.f,  -1,-1, 1, 0,1, .35f,1.f,1.f,
    };
    r.verts.assign(d, d + sizeof(d)/sizeof(float));
    return r;
}

// ── Primitive mesh generators ─────────────────────────────────────────────────
static LoadedMesh generate_primitive(const char* type) {
    if (std::string(type) == "cube") return make_unit_cube();

    LoadedMesh r;
    static constexpr float PI = 3.14159265358979f;

    if (std::string(type) == "plane") {
        r.bbox_min = {-1.f, 0.f, -1.f};
        r.bbox_max = { 1.f, 0.f,  1.f};
        const float c[] = {0.6f, 0.8f, 0.4f};
        auto push = [&](float x, float z, float u, float v) {
            r.verts.insert(r.verts.end(), {x, 0.f, z, u, v, c[0], c[1], c[2]});
        };
        push(-1,-1, 0,0); push( 1, 1, 1,1); push( 1,-1, 1,0);
        push(-1,-1, 0,0); push(-1, 1, 0,1); push( 1, 1, 1,1);
        return r;
    }

    if (std::string(type) == "sphere") {
        r.bbox_min = {-1.f,-1.f,-1.f};
        r.bbox_max = { 1.f, 1.f, 1.f};
        const int stacks = 16, slices = 16;
        const float c[] = {0.4f, 0.6f, 1.0f};
        auto push = [&](float lat, float lon) {
            float x = cosf(lat) * cosf(lon);
            float y = sinf(lat);
            float z = cosf(lat) * sinf(lon);
            float u = lon / (2.f * PI);
            float v = lat / PI + 0.5f;
            r.verts.insert(r.verts.end(), {x, y, z, u, v, c[0], c[1], c[2]});
        };
        for (int i = 0; i < stacks; ++i) {
            float lat0 = PI * (-0.5f + (float)i       / stacks);
            float lat1 = PI * (-0.5f + (float)(i + 1) / stacks);
            for (int j = 0; j < slices; ++j) {
                float lon0 = 2.f * PI * (float)j       / slices;
                float lon1 = 2.f * PI * (float)(j + 1) / slices;
                push(lat0, lon0); push(lat1, lon0); push(lat1, lon1);
                push(lat0, lon0); push(lat1, lon1); push(lat0, lon1);
            }
        }
        return r;
    }

    if (std::string(type) == "cylinder") {
        r.bbox_min = {-1.f,-1.f,-1.f};
        r.bbox_max = { 1.f, 1.f, 1.f};
        const int slices = 16;
        const float c[] = {1.0f, 0.7f, 0.3f};
        auto push = [&](float x, float y, float z, float u, float v) {
            r.verts.insert(r.verts.end(), {x, y, z, u, v, c[0], c[1], c[2]});
        };
        for (int j = 0; j < slices; ++j) {
            float a0 = 2.f * PI * (float)j       / slices;
            float a1 = 2.f * PI * (float)(j + 1) / slices;
            float x0 = cosf(a0), z0 = sinf(a0);
            float x1 = cosf(a1), z1 = sinf(a1);
            float u0 = (float)j / slices, u1 = (float)(j + 1) / slices;
            // side
            push(x0,-1,z0, u0,0); push(x1,-1,z1, u1,0); push(x1, 1,z1, u1,1);
            push(x0,-1,z0, u0,0); push(x1, 1,z1, u1,1); push(x0, 1,z0, u0,1);
            // top cap
            push(0, 1,0, 0.5f,0.5f); push(x0,1,z0, u0,0); push(x1,1,z1, u1,0);
            // bottom cap (reversed winding)
            push(0,-1,0, 0.5f,0.5f); push(x1,-1,z1, u1,0); push(x0,-1,z0, u0,0);
        }
        return r;
    }

    if (std::string(type) == "cone") {
        r.bbox_min = {-1.f,-1.f,-1.f};
        r.bbox_max = { 1.f, 1.f, 1.f};
        const int slices = 16;
        const float c[] = {0.9f, 0.4f, 0.8f};
        auto push = [&](float x, float y, float z, float u, float v) {
            r.verts.insert(r.verts.end(), {x, y, z, u, v, c[0], c[1], c[2]});
        };
        for (int j = 0; j < slices; ++j) {
            float a0 = 2.f * PI * (float)j       / slices;
            float a1 = 2.f * PI * (float)(j + 1) / slices;
            float x0 = cosf(a0), z0 = sinf(a0);
            float x1 = cosf(a1), z1 = sinf(a1);
            float u0 = (float)j / slices, u1 = (float)(j + 1) / slices;
            // side
            push(0, 1, 0, 0.5f,1.f); push(x0,-1,z0, u0,0); push(x1,-1,z1, u1,0);
            // base cap (reversed winding)
            push(0,-1, 0, 0.5f,0.5f); push(x1,-1,z1, u1,0); push(x0,-1,z0, u0,0);
        }
        return r;
    }

    if (std::string(type) == "capsule") {
        // Cylinder body height 1 (y in [-0.5, 0.5]) + two hemispheres
        // (radius 0.5) on top and bottom. Total y range [-1, 1] → bbox 1×2×1.
        r.bbox_min = {-0.5f, -1.f, -0.5f};
        r.bbox_max = { 0.5f,  1.f,  0.5f};
        const int   slices    = 8;
        const int   stacks    = 4;          // per hemisphere
        const float radius    = 0.5f;
        const float half_body = 0.5f;
        const float c[] = {0.7f, 0.9f, 0.3f};
        auto push = [&](float x, float y, float z, float u, float v) {
            r.verts.insert(r.verts.end(), {x, y, z, u, v, c[0], c[1], c[2]});
        };
        // Cylinder body
        for (int j = 0; j < slices; ++j) {
            float a0 = 2.f * PI * (float)j       / slices;
            float a1 = 2.f * PI * (float)(j + 1) / slices;
            float x0 = radius * cosf(a0), z0 = radius * sinf(a0);
            float x1 = radius * cosf(a1), z1 = radius * sinf(a1);
            float u0 = (float)j / slices, u1 = (float)(j + 1) / slices;
            push(x0,-half_body,z0, u0,0); push(x1,-half_body,z1, u1,0); push(x1, half_body,z1, u1,1);
            push(x0,-half_body,z0, u0,0); push(x1, half_body,z1, u1,1); push(x0, half_body,z0, u0,1);
        }
        // Top hemisphere (lat 0..PI/2, centered on y = +half_body)
        for (int i = 0; i < stacks; ++i) {
            float lat0 = (PI * 0.5f) * (float)i       / stacks;
            float lat1 = (PI * 0.5f) * (float)(i + 1) / stacks;
            for (int j = 0; j < slices; ++j) {
                float lon0 = 2.f * PI * (float)j       / slices;
                float lon1 = 2.f * PI * (float)(j + 1) / slices;
                auto p = [&](float lat, float lon) {
                    float x = radius * cosf(lat) * cosf(lon);
                    float y = half_body + radius * sinf(lat);
                    float z = radius * cosf(lat) * sinf(lon);
                    float u = lon / (2.f * PI);
                    float v = lat / PI + 0.5f;
                    push(x, y, z, u, v);
                };
                p(lat0, lon0); p(lat1, lon0); p(lat1, lon1);
                p(lat0, lon0); p(lat1, lon1); p(lat0, lon1);
            }
        }
        // Bottom hemisphere (lat -PI/2..0, centered on y = -half_body)
        for (int i = 0; i < stacks; ++i) {
            float lat0 = -(PI * 0.5f) + (PI * 0.5f) * (float)i       / stacks;
            float lat1 = -(PI * 0.5f) + (PI * 0.5f) * (float)(i + 1) / stacks;
            for (int j = 0; j < slices; ++j) {
                float lon0 = 2.f * PI * (float)j       / slices;
                float lon1 = 2.f * PI * (float)(j + 1) / slices;
                auto p = [&](float lat, float lon) {
                    float x = radius * cosf(lat) * cosf(lon);
                    float y = -half_body + radius * sinf(lat);
                    float z = radius * cosf(lat) * sinf(lon);
                    float u = lon / (2.f * PI);
                    float v = lat / PI + 0.5f;
                    push(x, y, z, u, v);
                };
                p(lat0, lon0); p(lat1, lon0); p(lat1, lon1);
                p(lat0, lon0); p(lat1, lon1); p(lat0, lon1);
            }
        }
        return r;
    }

    return make_unit_cube();  // fallback
}

// ── PSX framebuffer ───────────────────────────────────────────────────────────
struct PSXFramebuffer {
    static constexpr int W = 320, H = 240;
    GLuint fbo = 0, tex = 0, rbo = 0;

    void init() {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, W, H, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, rbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "PSX framebuffer incomplete!\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void bind()   { glBindFramebuffer(GL_FRAMEBUFFER, fbo); glViewport(0,0,W,H); }
    void unbind() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }
    void destroy() {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glDeleteRenderbuffers(1, &rbo);
    }
};

// ── Orbit camera ──────────────────────────────────────────────────────────────
struct OrbitCamera {
    glm::vec3 target = {0.f, 0.f, 0.f};
    float     radius = 5.f, yaw = 45.f, pitch = 25.f;
    bool      ortho  = false;

    glm::vec3 eye() const {
        float py = glm::radians(pitch), yr = glm::radians(yaw);
        return target + glm::vec3{
            radius * cosf(py) * cosf(yr),
            radius * sinf(py),
            radius * cosf(py) * sinf(yr)
        };
    }

    static glm::vec3 safe_up(glm::vec3 dir) {
        return fabsf(dir.y) > 0.99f ? glm::vec3{0,0,-1} : glm::vec3{0,1,0};
    }

    glm::mat4 view() const {
        glm::vec3 e = eye(), d = glm::normalize(target - e);
        return glm::lookAt(e, target, safe_up(d));
    }

    glm::mat4 proj(float aspect) const {
        if (ortho) {
            float h = radius;
            return glm::ortho(-h*aspect, h*aspect, -h, h, -200.f, 200.f);
        }
        return glm::perspective(glm::radians(60.f), aspect, 0.1f, 100.f);
    }

    void orbit(float dx, float dy) {
        yaw += dx * 0.4f; pitch += dy * 0.4f;
        pitch = glm::clamp(pitch, -89.f, 89.f);
    }

    void pan(float dx, float dy) {
        glm::vec3 e = eye(), fwd = glm::normalize(target - e);
        glm::vec3 right = glm::normalize(glm::cross(fwd, safe_up(fwd)));
        glm::vec3 up    = glm::cross(right, fwd);
        float spd = radius * 0.003f;
        target -= right * dx * spd;
        target += up    * dy * spd;
    }

    void zoom(int ticks) { radius -= ticks * radius * 0.15f; radius = glm::max(radius, 1.f); }
    void snap_front() { yaw =  90.f; pitch =  0.f; }
    void snap_side()  { yaw =   0.f; pitch =  0.f; }
    void snap_top()   { yaw =  90.f; pitch = 89.f; }
};

// ── Scene object ──────────────────────────────────────────────────────────────
struct SceneObject {
    std::string name;
    std::string mesh_path    = "assets/meshes/test.obj";
    std::string texture_path = "assets/textures/test.tga";
    bool        visible      = true;
    glm::vec3   position     = {0,0,0};
    glm::vec3   rotation     = {0,0,0};   // degrees
    glm::vec3   scale        = {1,1,1};
    std::string collision    = "box";
};

// ── Mesh cache ────────────────────────────────────────────────────────────────
struct MeshEntry {
    GLuint vao = 0, vbo = 0; int vert_count = 0;
    glm::vec3 bbox_min = {-1,-1,-1}, bbox_max = {1,1,1};
};

static MeshEntry upload_mesh(const LoadedMesh& lm) {
    MeshEntry me;
    me.vert_count = (int)lm.verts.size() / 8;
    me.bbox_min = lm.bbox_min; me.bbox_max = lm.bbox_max;
    if (!me.vert_count) return me;
    glGenVertexArrays(1, &me.vao); glGenBuffers(1, &me.vbo);
    glBindVertexArray(me.vao);
    glBindBuffer(GL_ARRAY_BUFFER, me.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(lm.verts.size()*sizeof(float)),
                 lm.verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(3*sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(5*sizeof(float)));
    glBindVertexArray(0);
    return me;
}

static std::vector<float> make_box_wire(glm::vec3 mn, glm::vec3 mx) {
    const glm::vec3 c[8] = {
        {mn.x,mn.y,mn.z},{mx.x,mn.y,mn.z},{mx.x,mx.y,mn.z},{mn.x,mx.y,mn.z},
        {mn.x,mn.y,mx.z},{mx.x,mn.y,mx.z},{mx.x,mx.y,mx.z},{mn.x,mx.y,mx.z},
    };
    const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    std::vector<float> out; out.reserve(24*8);
    for (auto& ed : e)
        for (int k = 0; k < 2; ++k) {
            const glm::vec3& p = c[ed[k]];
            out.insert(out.end(), {p.x,p.y,p.z, 0,0, 1,1,1});
        }
    return out;
}

// ── Hierarchy helpers ────────────────────────────────────────────────────────
// A node is visible only if every ancestor up to the root is also visible.
// A toggle on the Level root therefore hides the entire subtree.
static bool node_visible(const psx::Scene& s, int node_id) {
    const psx::Node* n = s.find(node_id);
    if (!n)              return false;
    if (!n->visible)     return false;
    if (n->parent < 0)   return true;
    return node_visible(s, n->parent);
}

static glm::mat4 world_matrix(const psx::Scene& s, int node_id) {
    const psx::Node* n = s.find(node_id);
    if (!n) return glm::mat4(1.f);
    glm::mat4 local = glm::translate(glm::mat4(1.f), n->position);
    local = local * glm::mat4_cast(n->rotation);
    local = glm::scale(local, n->scale);
    if (n->parent < 0) return local;
    return world_matrix(s, n->parent) * local;
}

static glm::mat4 parent_world(const psx::Scene& s, int node_id) {
    const psx::Node* n = s.find(node_id);
    if (!n || n->parent < 0) return glm::mat4(1.f);
    return world_matrix(s, n->parent);
}

// Like world_matrix but skips THIS node's own scale (parent scales still apply).
// Used for indicator gizmos where size comes from a component value, not the
// node transform.
static glm::mat4 world_matrix_no_self_scale(const psx::Scene& s, int node_id) {
    const psx::Node* n = s.find(node_id);
    if (!n) return glm::mat4(1.f);
    glm::mat4 m = parent_world(s, node_id);
    m = glm::translate(m, n->position);
    m = m * glm::mat4_cast(n->rotation);
    return m;
}

// Decompose a TRS matrix into translation, rotation (as quaternion), and
// scale. The rotation block is normalised before glm::quat_cast.
static void decompose_trs_q(const glm::mat4& m, glm::vec3& t,
                            glm::quat& q, glm::vec3& s) {
    t = glm::vec3(m[3]);
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    s.x = glm::length(c0);
    s.y = glm::length(c1);
    s.z = glm::length(c2);
    glm::mat3 R;
    R[0] = (s.x > 1e-6f) ? c0 / s.x : glm::vec3(1.f, 0.f, 0.f);
    R[1] = (s.y > 1e-6f) ? c1 / s.y : glm::vec3(0.f, 1.f, 0.f);
    R[2] = (s.z > 1e-6f) ? c2 / s.z : glm::vec3(0.f, 0.f, 1.f);
    q = glm::quat_cast(R);
}

// ── Wire-shape generators (line lists, 8 floats per vert: pos+uv+colour pad) ──
static void wire_push(std::vector<float>& v, glm::vec3 p) {
    v.insert(v.end(), {p.x, p.y, p.z, 0.f, 0.f, 1.f, 1.f, 1.f});
}

static std::vector<float> make_camera_wire() {
    std::vector<float> v;
    glm::vec3 apex(0.f, 0.f, 0.f);
    glm::vec3 c0(-0.2f, -0.15f, -0.5f);
    glm::vec3 c1( 0.2f, -0.15f, -0.5f);
    glm::vec3 c2( 0.2f,  0.15f, -0.5f);
    glm::vec3 c3(-0.2f,  0.15f, -0.5f);
    // Apex → 4 base corners
    wire_push(v, apex); wire_push(v, c0);
    wire_push(v, apex); wire_push(v, c1);
    wire_push(v, apex); wire_push(v, c2);
    wire_push(v, apex); wire_push(v, c3);
    // Base loop
    wire_push(v, c0); wire_push(v, c1);
    wire_push(v, c1); wire_push(v, c2);
    wire_push(v, c2); wire_push(v, c3);
    wire_push(v, c3); wire_push(v, c0);
    // "Up" tent above the top edge
    glm::vec3 tip(0.f, 0.25f, -0.5f);
    wire_push(v, c3); wire_push(v, tip);
    wire_push(v, tip); wire_push(v, c2);
    return v;
}

static std::vector<float> make_dir_light_wire() {
    std::vector<float> v;
    // Square in the XZ plane at y=0
    glm::vec3 a(-0.25f, 0.f, -0.25f);
    glm::vec3 b( 0.25f, 0.f, -0.25f);
    glm::vec3 c( 0.25f, 0.f,  0.25f);
    glm::vec3 d(-0.25f, 0.f,  0.25f);
    wire_push(v, a); wire_push(v, b);
    wire_push(v, b); wire_push(v, c);
    wire_push(v, c); wire_push(v, d);
    wire_push(v, d); wire_push(v, a);
    // Arrow: shaft (0,0,0) → (0,-1,0) plus arrowhead
    glm::vec3 top(0.f, 0.f, 0.f), bot(0.f, -1.f, 0.f);
    wire_push(v, top); wire_push(v, bot);
    wire_push(v, bot); wire_push(v, glm::vec3(-0.1f, -0.85f, 0.f));
    wire_push(v, bot); wire_push(v, glm::vec3( 0.1f, -0.85f, 0.f));
    return v;
}

static std::vector<float> make_sphere_wire() {
    // Three orthogonal great circles, 16 segments each.
    std::vector<float> v;
    static constexpr float PI2 = 6.28318530718f;
    const int seg = 16;
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < seg; ++i) {
            float a0 = PI2 * (float)i       / seg;
            float a1 = PI2 * (float)(i + 1) / seg;
            glm::vec3 p0, p1;
            if (axis == 0)      { p0 = {0.f, cosf(a0), sinf(a0)}; p1 = {0.f, cosf(a1), sinf(a1)}; }
            else if (axis == 1) { p0 = {cosf(a0), 0.f, sinf(a0)}; p1 = {cosf(a1), 0.f, sinf(a1)}; }
            else                { p0 = {cosf(a0), sinf(a0), 0.f}; p1 = {cosf(a1), sinf(a1), 0.f}; }
            wire_push(v, p0); wire_push(v, p1);
        }
    }
    return v;
}

static std::vector<float> make_cone_wire() {
    // Apex at (0,0,0), unit cone pointing -Y, base at y=-1, radius 1.
    std::vector<float> v;
    static constexpr float PI2 = 6.28318530718f;
    const int seg = 8;
    glm::vec3 apex(0.f, 0.f, 0.f);
    glm::vec3 base[8];
    for (int i = 0; i < seg; ++i) {
        float a = PI2 * (float)i / seg;
        base[i] = {cosf(a), -1.f, sinf(a)};
    }
    for (int i = 0; i < seg; ++i) {
        wire_push(v, base[i]);
        wire_push(v, base[(i + 1) % seg]);
    }
    // 4 spokes from apex to every other base vertex.
    for (int i = 0; i < 4; ++i) {
        wire_push(v, apex);
        wire_push(v, base[i * 2]);
    }
    return v;
}

// ── Scene I/O ─────────────────────────────────────────────────────────────────
// The v2 reader/writer lives in shared/scene_format.hpp. It serialises the
// full psx::Scene tree (kinds, parent/child links, components), so saving a
// Player rigged with a Camera + Capsule round-trips losslessly. Loading
// auto-migrates v1 ("objects" array) documents on the fly.

// ── File picker ───────────────────────────────────────────────────────────────
struct FilePicker {
    std::string* dest = nullptr; std::vector<std::string> files;
    void open(std::string* dst, const char* dir,
              std::initializer_list<const char*> exts) {
        dest = dst; files.clear();
        namespace fs = std::filesystem; std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = (char)tolower((unsigned char)c);
            for (const char* e : exts)
                if (ext == e) { files.push_back(entry.path().string()); break; }
        }
        std::sort(files.begin(), files.end());
        ImGui::OpenPopup("File Picker");
    }
};

// ── Entry point ───────────────────────────────────────────────────────────────
int main(int /*argc*/, char** /*argv*/) {
    // ── Default assets ────────────────────────────────────────────────────────
    {
        namespace fs = std::filesystem;
        fs::create_directories("assets/textures");
        if (!fs::exists("assets/textures/test.tga"))
            write_checkerboard_tga("assets/textures/test.tga", 64, 8);
        const char* mp = "assets/meshes/test.obj";
        bool regen = true;
        if (fs::exists(mp)) {
            std::ifstream probe(mp, std::ios::binary);
            std::string head(4096,'\0'); probe.read(head.data(),4096);
            head.resize((size_t)probe.gcount());
            regen = head.find("vt ") == std::string::npos;
        }
        if (regen) {
            fs::create_directories("assets/meshes");
            std::ofstream mf(mp);
            if (mf) {
                mf << "# test cube\n"
                   << "v -1.0 -1.0  1.0\nv  1.0 -1.0  1.0\n"
                   << "v  1.0  1.0  1.0\nv -1.0  1.0  1.0\n"
                   << "v -1.0 -1.0 -1.0\nv  1.0 -1.0 -1.0\n"
                   << "v  1.0  1.0 -1.0\nv -1.0  1.0 -1.0\n"
                   << "vt 0.0 0.0\nvt 1.0 0.0\nvt 1.0 1.0\nvt 0.0 1.0\n"
                   << "f 1/1 2/2 3/3 4/4\nf 6/1 5/2 8/3 7/4\n"
                   << "f 5/1 1/2 4/3 8/4\nf 2/1 6/2 7/3 3/4\n"
                   << "f 4/1 3/2 7/3 8/4\nf 5/1 6/2 2/3 1/4\n";
            }
        }
    }

    // ── SDL / GL ──────────────────────────────────────────────────────────────
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)!=0) {
        fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* win = SDL_CreateWindow("psx-editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr,"SDL_CreateWindow: %s\n",SDL_GetError()); return 1; }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr,"SDL_GL_CreateContext: %s\n",SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        fprintf(stderr,"GLAD: failed\n"); return 1;
    }
    fprintf(stderr,"OpenGL %s  |  %s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    PSXFramebuffer psx_fb; psx_fb.init();

    // ── Wire shader ───────────────────────────────────────────────────────────
    static const char* wire_vert = R"(
#version 330 core
layout(location=0) in vec3 a_pos;
uniform mat4 u_mvp;
void main(){gl_Position=u_mvp*vec4(a_pos,1.0);}
)";
    static const char* wire_frag = R"(
#version 330 core
out vec4 frag_color;
uniform vec3 u_wire_color;
void main(){frag_color=vec4(u_wire_color,1.0);}
)";
    GLuint wire_prog = link_program(compile_shader(GL_VERTEX_SHADER,wire_vert),
                                    compile_shader(GL_FRAGMENT_SHADER,wire_frag));
    GLint u_wire_mvp = glGetUniformLocation(wire_prog,"u_mvp");
    GLint u_wire_col = glGetUniformLocation(wire_prog,"u_wire_color");

    // wire_vbo is shared between the box-collision overlay and the per-kind
    // gizmo overlays (camera/light). Sized for the largest user (sphere wire =
    // 96 verts).
    GLuint wire_vao=0, wire_vbo=0;
    glGenVertexArrays(1,&wire_vao); glGenBuffers(1,&wire_vbo);
    glBindVertexArray(wire_vao);
    glBindBuffer(GL_ARRAY_BUFFER,wire_vbo);
    glBufferData(GL_ARRAY_BUFFER,128*8*sizeof(float),nullptr,GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)0);
    glBindVertexArray(0);

    const std::vector<float> camera_wire_verts      = make_camera_wire();
    const std::vector<float> dir_light_wire_verts   = make_dir_light_wire();
    const std::vector<float> point_light_wire_verts = make_sphere_wire();
    const std::vector<float> spot_light_wire_verts  = make_cone_wire();

    // ── PSX mesh shader ───────────────────────────────────────────────────────
    auto load_prog = [&](const char* v, const char* f) {
        std::string vs=read_file(v), fs=read_file(f);
        return link_program(compile_shader(GL_VERTEX_SHADER,vs.c_str()),
                            compile_shader(GL_FRAGMENT_SHADER,fs.c_str()));
    };
    GLuint mesh_prog = load_prog("shaders/psx.vert","shaders/psx.frag");
    GLint u_mvp     = glGetUniformLocation(mesh_prog,"u_mvp");
    GLint u_snap    = glGetUniformLocation(mesh_prog,"u_snap_resolution");
    GLint u_use_tex = glGetUniformLocation(mesh_prog,"u_use_texture");
    GLint u_tex     = glGetUniformLocation(mesh_prog,"u_texture");

    // ── Project / scene paths ────────────────────────────────────────────────
    // current_project_path: full filesystem path to the loaded project.psxproj,
    //                       empty = free-floating mode.
    // current_scene_path  : in project mode, relative to project root
    //                       (e.g. "scenes/level1.pscene"); in free-floating
    //                       mode, the path passed to save/load directly.
    //                       Empty = unsaved.
    std::string  current_project_path;
    std::string  current_scene_path;
    psx::Project current_project;

    auto project_root_dir = [&]() -> std::string {
        if (current_project_path.empty()) return "";
        return std::filesystem::path(current_project_path)
                   .parent_path().string();
    };

    auto assets_dir = [&](const char* sub) -> std::string {
        if (current_project_path.empty())
            return std::string("assets/") + sub + "/";
        return project_root_dir() + "/assets/" + sub + "/";
    };

    // Stored asset path → real filesystem path. Built-in primitives and
    // absolute paths pass through; in project mode, relative paths are
    // resolved against the project root.
    auto resolve_asset = [&](const std::string& p) -> std::string {
        if (p.empty() || p.rfind("__primitive_", 0) == 0) return p;
        if (current_project_path.empty()) return p;
        namespace fs = std::filesystem;
        fs::path pp(p);
        if (pp.is_absolute()) return p;
        return (fs::path(project_root_dir()) / pp).generic_string();
    };

    // Picked file path (absolute) → project-relative form for storage.
    auto rel_to_project = [&](const std::string& abs) -> std::string {
        if (current_project_path.empty()) return abs;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path rel = fs::relative(abs, project_root_dir(), ec);
        if (ec || rel.empty()) return abs;
        return rel.generic_string();
    };

    auto resolve_scene_path = [&](const std::string& p) -> std::string {
        if (p.empty()) return "";
        if (current_project_path.empty()) return p;
        namespace fs = std::filesystem;
        fs::path pp(p);
        if (pp.is_absolute()) return p;
        return (fs::path(project_root_dir()) / pp).generic_string();
    };

    // ── Caches ────────────────────────────────────────────────────────────────
    std::map<std::string,MeshEntry>              mesh_cache;
    std::map<std::string,GLuint>                 tex_cache;
    std::map<std::string,std::vector<std::string>> validation_warnings;

    // Pre-seed built-in primitives so get_mesh never tries to load them from disk
    {
        struct { const char* key; const char* type; } prims[] = {
            {"__primitive_cube__",     "cube"},
            {"__primitive_plane__",    "plane"},
            {"__primitive_sphere__",   "sphere"},
            {"__primitive_cylinder__", "cylinder"},
            {"__primitive_cone__",     "cone"},
            {"__primitive_capsule__",  "capsule"},
        };
        for (auto& p : prims)
            mesh_cache[p.key] = upload_mesh(generate_primitive(p.type));
    }

    auto get_mesh = [&](const std::string& path) -> MeshEntry& {
        auto it = mesh_cache.find(path);
        if (it != mesh_cache.end()) return it->second;
        const std::string fs_path = resolve_asset(path);
        LoadedMesh lm = load_obj(fs_path.c_str());
        // PSX geometry validation (OBJ only — primitives are pre-seeded and skip this)
        std::vector<std::string>& warns = validation_warnings[path];
        warns.clear();
        if (!lm.verts.empty()) {
            int tri_count = (int)lm.verts.size() / 8 / 3;
            if (tri_count > 500)
                warns.push_back("High poly: " + std::to_string(tri_count)
                                + " tris (PSX target <500)");
            bool uv_oob = false;
            for (int i = 0; i + 7 < (int)lm.verts.size() && !uv_oob; i += 8) {
                float u = lm.verts[i + 3], v = lm.verts[i + 4];
                if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) uv_oob = true;
            }
            if (uv_oob) warns.push_back("UVs outside 0-1 range (affine warp may swim)");
        }
        if (lm.verts.empty()) lm = make_unit_cube();
        mesh_cache[path] = upload_mesh(lm);
        return mesh_cache[path];
    };

    // Drop OBJ + TGA caches when the project root changes; primitives stay
    // because their keys (__primitive_*__) don't depend on the project.
    auto clear_asset_caches = [&]() {
        for (auto it = mesh_cache.begin(); it != mesh_cache.end(); ) {
            if (it->first.rfind("__primitive_", 0) == 0) { ++it; continue; }
            if (it->second.vao) glDeleteVertexArrays(1, &it->second.vao);
            if (it->second.vbo) glDeleteBuffers(1, &it->second.vbo);
            it = mesh_cache.erase(it);
        }
        for (auto& [k, t] : tex_cache) if (t) glDeleteTextures(1, &t);
        tex_cache.clear();
        validation_warnings.clear();
    };

    auto get_tex = [&](const std::string& path) -> GLuint {
        auto it = tex_cache.find(path);
        if (it != tex_cache.end()) return it->second;
        TGAImage img = load_tga(resolve_asset(path).c_str());
        GLuint tex = 0;
        if (!img.rgb.empty()) {
            glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,img.w,img.h,0,
                         GL_RGB,GL_UNSIGNED_BYTE,img.rgb.data());
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
            glBindTexture(GL_TEXTURE_2D,0);
        }
        tex_cache[path] = tex; return tex;
    };

    SDL_Cursor* cur_arrow     = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    SDL_Cursor* cur_crosshair = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    SDL_Cursor* cur_sizeall   = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);

    // ── Scene state ───────────────────────────────────────────────────────────
    // scene is the structural truth (tree, kinds, components, node IDs).
    // scene_objects is a derived mirror of mesh-kind nodes only — kept in
    // scene.nodes declaration order — so the renderer + gizmo can keep
    // using their existing shape until step 6 swaps them onto the tree.
    std::vector<SceneObject> scene_objects = {{"Cube"},{"Light"},{"Spawn Point"}};
    psx::Scene scene;
    int         selected_node_id = -1;

    // i-th mesh-kind node in scene.nodes order, or -1 if node_id isn't a mesh.
    auto mesh_index_for_node = [&](int node_id) -> int {
        int j = 0;
        for (const auto& n : scene.nodes) {
            if (n.kind != "mesh") continue;
            if (n.id == node_id) return j;
            ++j;
        }
        return -1;
    };
    // -1 unless the selected node is a mesh kind. Inspector/gizmo gate on this.
    auto sel_idx = [&]() -> int { return mesh_index_for_node(selected_node_id); };

    // Rebuild scene_objects to mirror current mesh nodes (scene.nodes order).
    auto sync_objects_from_scene = [&]() {
        scene_objects.clear();
        for (const auto& n : scene.nodes) {
            if (n.kind != "mesh") continue;
            SceneObject obj;
            obj.name     = n.name;
            obj.position = n.position;
            obj.rotation = glm::degrees(glm::eulerAngles(n.rotation));
            obj.scale    = n.scale;
            obj.visible  = n.visible;
            nlohmann::json m = n.components.value("mesh", nlohmann::json::object());
            obj.mesh_path    = m.value("path",    std::string(""));
            obj.texture_path = m.value("texture", std::string(""));
            nlohmann::json c = n.components.value("collision", nlohmann::json::object());
            obj.collision    = c.value("type", std::string("box"));
            scene_objects.push_back(std::move(obj));
        }
    };

    // Build a fresh v1-shaped scene from scene_objects (used at startup and on
    // .pscene v1 load — only mesh data exists in that path).
    auto sync_scene_from_objects = [&]() {
        scene = psx::Scene{};
        scene.name = "untitled";
        psx::Node root;
        root.name = "Level"; root.kind = "node"; root.parent = -1;
        int root_id = scene.add_node(std::move(root));
        for (const auto& obj : scene_objects) {
            psx::Node n;
            n.name     = obj.name;
            n.kind     = "mesh";
            n.parent   = root_id;
            n.position = obj.position;
            n.rotation = glm::quat(glm::radians(obj.rotation));
            n.scale    = obj.scale;
            n.visible  = obj.visible;
            n.components["mesh"]["path"]      = obj.mesh_path;
            n.components["mesh"]["texture"]   = obj.texture_path;
            n.components["collision"]["type"] = obj.collision;
            scene.add_node(std::move(n));
        }
    };
    sync_scene_from_objects();
    std::string status_msg;
    Uint32      status_until  = 0;
    FilePicker  picker;

    // Project / scene file modals — populated by File-menu handlers, drained
    // each frame in the modal-render block below.
    bool open_new_proj_modal_request   = false;
    bool open_open_proj_modal_request  = false;
    bool open_scene_picker_request     = false;
    bool open_save_scene_as_request    = false;
    char modal_new_proj_parent[512]    = "";
    char modal_new_proj_name[256]      = "my_game";
    char modal_open_proj_path[512]     = "";
    char modal_save_scene_as[256]      = "";
    std::vector<std::string> scene_picker_files;
    FsBrowser fs_browser;
    int         gizmo_op      = 0;      // 0=translate 1=rotate 2=scale
    enum        { GIZMO_GLOBAL = 0, GIZMO_LOCAL = 1 };
    int         gizmo_mode    = GIZMO_GLOBAL;
    bool        snap_enabled    = false;
    float       snap_translate  = 0.5f;
    float       snap_rotate     = 15.f;
    float       snap_scale      = 0.1f;
    // Per-node "preferred Euler" (degrees) for the Inspector — derived from the
    // quat on first display and held stable while the user types, so the Euler
    // triple doesn't jump as the underlying quaternion is re-decomposed.
    std::map<int, glm::vec3> preferred_euler_deg;
    bool        scene_dirty   = false;
    std::string last_title;

    // ── Undo / redo ───────────────────────────────────────────────────────────
    struct Snapshot { psx::Scene scene; int selected_node_id; };
    static constexpr int MAX_UNDO = 50;
    std::vector<Snapshot> undo_stack, redo_stack;

    auto push_undo = [&]() {
        undo_stack.push_back({scene, selected_node_id});
        if ((int)undo_stack.size() > MAX_UNDO)
            undo_stack.erase(undo_stack.begin());
        redo_stack.clear();
    };

    auto do_undo = [&]() {
        if (undo_stack.empty()) return;
        redo_stack.push_back({scene, selected_node_id});
        scene            = undo_stack.back().scene;
        selected_node_id = undo_stack.back().selected_node_id;
        undo_stack.pop_back();
        sync_objects_from_scene();
        preferred_euler_deg.clear();
        scene_dirty = true;
    };

    auto do_redo = [&]() {
        if (redo_stack.empty()) return;
        undo_stack.push_back({scene, selected_node_id});
        scene            = redo_stack.back().scene;
        selected_node_id = redo_stack.back().selected_node_id;
        redo_stack.pop_back();
        sync_objects_from_scene();
        preferred_euler_deg.clear();
        scene_dirty = true;
    };

    // ── Scene actions ─────────────────────────────────────────────────────────
    // do_save_scene_to() writes the FULL scene tree (psx::Scene) — every
    // node kind with its parent/child links and components — using the v2
    // serialiser from shared/scene_format.hpp. The legacy scene_objects
    // mirror is not consulted on save.
    auto do_save_scene_to = [&](const std::string& path) {
        if (path.empty()) return;
        const std::string fs_path = resolve_scene_path(path);
        scene.format_version = 2; // belt-and-braces — to_json reads this field
        try {
            psx::save_scene(scene, fs_path);
        } catch (const std::exception& e) {
            status_msg   = std::string("Error: ") + e.what();
            status_until = SDL_GetTicks() + 2500;
            fprintf(stderr, "scene: save failed: %s\n", e.what());
            return;
        }
        fprintf(stderr, "Saved v2 scene: %d nodes -> %s\n",
                (int)scene.nodes.size(), fs_path.c_str());
        scene_dirty        = false;
        current_scene_path = path;
        status_msg         = "Saved.";
        status_until       = SDL_GetTicks() + 2000;
    };

    auto do_save_scene = [&]() {
        // Free-floating with no path → fall back to "scene.pscene" (legacy
        // default). With a project, fall back to "scenes/untitled.pscene".
        if (current_scene_path.empty()) {
            current_scene_path = current_project_path.empty()
                                     ? "scene.pscene"
                                     : "scenes/untitled.pscene";
        }
        do_save_scene_to(current_scene_path);
    };

    // Loads either v2 ("nodes" / format_version==2) or v1 ("objects" array,
    // auto-migrated). After load: scene_objects mirror is rebuilt, selection
    // cleared, dirty flag reset, undo stacks dropped.
    auto do_load_scene_from = [&](const std::string& path) {
        if (path.empty()) return;
        const std::string fs_path = resolve_scene_path(path);
        std::ifstream f(fs_path);
        if (!f) {
            status_msg   = "Error: cannot open scene.";
            status_until = SDL_GetTicks() + 2500;
            fprintf(stderr, "scene: cannot open %s\n", fs_path.c_str());
            return;
        }
        nlohmann::json doc;
        try { doc = nlohmann::json::parse(f); }
        catch (const std::exception& e) {
            status_msg   = std::string("Error: ") + e.what();
            status_until = SDL_GetTicks() + 2500;
            fprintf(stderr, "scene: parse failed: %s\n", e.what());
            return;
        }
        psx::Scene loaded;
        const bool is_v2 =
            doc.value("format_version", 0) == 2 || doc.contains("nodes");
        const bool is_v1 = !is_v2 && doc.contains("objects");
        if (is_v2) {
            try { loaded = doc.get<psx::Scene>(); }
            catch (const std::exception& e) {
                status_msg   = std::string("Error: ") + e.what();
                status_until = SDL_GetTicks() + 2500;
                fprintf(stderr, "scene: v2 deserialise failed: %s\n", e.what());
                return;
            }
            fprintf(stderr, "Loaded v2 scene: %d nodes <- %s\n",
                    (int)loaded.nodes.size(), fs_path.c_str());
        } else if (is_v1) {
            loaded = psx::Scene::from_v1(doc);
            fprintf(stderr, "Migrated v1 scene: %d nodes <- %s\n",
                    (int)loaded.nodes.size(), fs_path.c_str());
        } else {
            status_msg   = "Error: unknown scene format.";
            status_until = SDL_GetTicks() + 2500;
            fprintf(stderr, "scene: unknown format in %s\n", fs_path.c_str());
            return;
        }

        scene = std::move(loaded);
        sync_objects_from_scene();
        selected_node_id   = -1;
        preferred_euler_deg.clear();
        scene_dirty        = false;
        current_scene_path = path;
        undo_stack.clear();
        redo_stack.clear();
        status_msg   = "Loaded.";
        status_until = SDL_GetTicks() + 2000;
    };

    auto do_new_scene = [&]() {
        push_undo();
        scene_objects.clear();
        selected_node_id = -1;
        sync_scene_from_objects();
        preferred_euler_deg.clear();
        scene_dirty        = false;
        current_scene_path = "";
    };

    // ── Project actions ───────────────────────────────────────────────────────
    auto do_new_project = [&](const std::string& parent_dir,
                              const std::string& proj_name) -> bool {
        namespace fs = std::filesystem;
        if (parent_dir.empty() || proj_name.empty()) return false;
        fs::path root = fs::path(parent_dir) / proj_name;
        std::error_code ec;
        if (fs::exists(root, ec)) {
            status_msg   = "Error: project folder already exists.";
            status_until = SDL_GetTicks() + 2500;
            return false;
        }
        if (!fs::create_directories(root / "assets" / "meshes", ec) || ec) {
            status_msg   = "Error.";
            status_until = SDL_GetTicks() + 2000;
            return false;
        }
        fs::create_directories(root / "assets" / "textures", ec);
        fs::create_directories(root / "scenes", ec);

        psx::Project proj;
        proj.name          = proj_name;
        proj.default_scene = "scenes/untitled.pscene";
        const std::string proj_path = (root / "project.psxproj").string();
        try { psx::save_project(proj, proj_path); }
        catch (const std::exception& e) {
            status_msg   = std::string("Error: ") + e.what();
            status_until = SDL_GetTicks() + 2500;
            return false;
        }

        current_project      = proj;
        current_project_path = proj_path;
        clear_asset_caches();
        do_new_scene();
        current_scene_path = proj.default_scene;
        do_save_scene_to(proj.default_scene);

        status_msg   = "Project created.";
        status_until = SDL_GetTicks() + 2000;
        return true;
    };

    auto do_open_project = [&](const std::string& path) -> bool {
        try {
            psx::Project p = psx::load_project(path);
            current_project      = p;
            current_project_path = path;
            clear_asset_caches();
            if (!p.default_scene.empty())
                do_load_scene_from(p.default_scene);
            else
                do_new_scene();
            status_msg   = "Project opened.";
            status_until = SDL_GetTicks() + 2000;
            return true;
        } catch (const std::exception& e) {
            status_msg   = std::string("Error: ") + e.what();
            status_until = SDL_GetTicks() + 2500;
            return false;
        }
    };

    auto do_close_project = [&]() {
        current_project      = psx::Project{};
        current_project_path = "";
        clear_asset_caches();
        do_new_scene();
        status_msg   = "Project closed.";
        status_until = SDL_GetTicks() + 2000;
    };

    // Add a new node under the currently-selected node (or the scene root if
    // nothing is selected). Returns the freshly-assigned node id.
    auto add_under_selected = [&](psx::Node n) -> int {
        int parent_id = (selected_node_id >= 0 && scene.find(selected_node_id))
                        ? selected_node_id : 0;
        n.parent = parent_id;
        return scene.add_node(std::move(n));
    };

    // "Add Object" menubar item: append a default mesh-from-file node.
    auto do_add = [&]() {
        push_undo();
        psx::Node n;
        n.name = "Object";
        n.kind = "mesh";
        n.components["mesh"]["path"]      = "assets/meshes/test.obj";
        n.components["mesh"]["texture"]   = "assets/textures/test.tga";
        n.components["collision"]["type"] = "box";
        selected_node_id = add_under_selected(std::move(n));
        sync_objects_from_scene();
        scene_dirty = true;
    };

    auto do_dup = [&]() {
        if (sel_idx() < 0) return;                   // mesh-only for now
        psx::Node* original = scene.find(selected_node_id);
        if (!original) return;
        push_undo();
        psx::Node copy = *original;                  // deep copy of fields + json
        copy.children.clear();                       // children not cloned in step 3
        copy.name       += " (copy)";
        copy.position.x += 0.5f;
        copy.position.z += 0.5f;
        // copy.parent inherits from original; add_node wires it into that parent.
        selected_node_id = scene.add_node(std::move(copy));
        sync_objects_from_scene();
        scene_dirty = true;
    };

    // Reparent dragged_id under new_parent_id, preserving world pose.
    // Returns true if the reparent happened.
    auto reparent = [&](int dragged_id, int new_parent_id) -> bool {
        if (dragged_id == new_parent_id) return false;     // onto self
        if (dragged_id == 0)             return false;     // root not movable
        // Cycle: walk up from new_parent; if we encounter dragged_id, reject.
        for (int cur = new_parent_id; cur >= 0; ) {
            if (cur == dragged_id) return false;
            const psx::Node* p = scene.find(cur);
            if (!p) break;
            cur = p->parent;
        }
        psx::Node* dragged    = scene.find(dragged_id);
        psx::Node* new_parent = scene.find(new_parent_id);
        if (!dragged || !new_parent) return false;
        if (dragged->parent == new_parent_id) return false; // already there

        push_undo();

        // Snapshot world pose, then rewire parent/children.
        glm::mat4 old_world = world_matrix(scene, dragged_id);

        if (dragged->parent >= 0) {
            if (psx::Node* old_parent = scene.find(dragged->parent)) {
                auto& ch = old_parent->children;
                ch.erase(std::remove(ch.begin(), ch.end(), dragged_id), ch.end());
            }
        }
        dragged->parent = new_parent_id;
        new_parent->children.push_back(dragged_id);

        // Convert world pose to local under the new parent.
        glm::mat4 new_pw    = world_matrix(scene, new_parent_id);
        glm::mat4 new_local = glm::inverse(new_pw) * old_world;
        glm::vec3 t, s;
        glm::quat q;
        decompose_trs_q(new_local, t, q, s);
        dragged->position = t;
        dragged->rotation = q;
        dragged->scale    = s;
        // Refresh the preferred-Euler entry so the Inspector shows the new
        // local rotation rather than what was typed under the old parent.
        preferred_euler_deg[dragged_id] =
            glm::degrees(glm::eulerAngles(q));

        sync_objects_from_scene();
        scene_dirty = true;
        return true;
    };

    OrbitCamera cam;
    bool orbit_active = false, pan_active = false, running = true;
    bool gizmo_was_using = false;

    while (running) {
        float mouse_dx = 0.f, mouse_dy = 0.f;
        int   scroll_ticks = 0;
        bool mmb_pressed=false, mmb_released=false, shift_at_press=false;
        bool key_np1=false, key_np3=false, key_np5=false, key_np7=false;
        bool key_del=false, key_save=false, key_load=false;
        bool key_t=false, key_r=false, key_s_gizmo=false;
        bool key_undo=false, key_redo=false, key_dup=false;
        bool key_x_toggle=false;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                running = false;
            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_MIDDLE) {
                mmb_pressed = true;
                shift_at_press = (SDL_GetModState() & KMOD_SHIFT) != 0;
            }
            if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_MIDDLE)
                mmb_released = true;
            if (ev.type == SDL_MOUSEMOTION) {
                mouse_dx += (float)ev.motion.xrel;
                mouse_dy += (float)ev.motion.yrel;
            }
            if (ev.type == SDL_MOUSEWHEEL) scroll_ticks += ev.wheel.y;
            if (ev.type == SDL_KEYDOWN && !io.WantCaptureKeyboard) {
                SDL_Keymod mod = (SDL_Keymod)ev.key.keysym.mod;
                switch (ev.key.keysym.sym) {
                    case SDLK_KP_1: key_np1 = true; break;
                    case SDLK_KP_3: key_np3 = true; break;
                    case SDLK_KP_5: key_np5 = true; break;
                    case SDLK_KP_7: key_np7 = true; break;
                    case SDLK_DELETE: key_del = true; break;
                    case SDLK_s:
                        if (mod & KMOD_CTRL) key_save = true;
                        else key_s_gizmo = true;
                        break;
                    case SDLK_o: if (mod & KMOD_CTRL) key_load = true; break;
                    case SDLK_z:
                        if (mod & KMOD_CTRL) {
                            if (mod & KMOD_SHIFT) key_redo = true;
                            else                  key_undo = true;
                        }
                        break;
                    case SDLK_y: if (mod & KMOD_CTRL) key_redo = true; break;
                    case SDLK_d: if (mod & KMOD_CTRL) key_dup  = true; break;
                    case SDLK_t: key_t = true; break;
                    case SDLK_r: key_r = true; break;
                    case SDLK_x: key_x_toggle = true; break;
                    default: break;
                }
            }
        }

        if (key_t)       gizmo_op = 0;
        if (key_r)       gizmo_op = 1;
        if (key_s_gizmo) gizmo_op = 2;
        if (key_x_toggle && selected_node_id >= 0)
            gizmo_mode = (gizmo_mode == GIZMO_GLOBAL) ? GIZMO_LOCAL : GIZMO_GLOBAL;
        if (key_undo)    do_undo();
        if (key_redo)    do_redo();
        if (key_dup)     do_dup();
        if (key_save)    do_save_scene();
        if (key_load)    do_load_scene_from(
                             current_scene_path.empty()
                                 ? std::string("scene.pscene")
                                 : current_scene_path);

        // ── PSX render ───────────────────────────────────────────────────────
        float     aspect = (float)PSXFramebuffer::W / (float)PSXFramebuffer::H;
        glm::mat4 vp     = cam.proj(aspect) * cam.view();

        psx_fb.bind();
        glClearColor(0.05f,0.03f,0.12f,1.f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        glUseProgram(mesh_prog);
        glUniform1f(u_snap, 60.f);
        for (const psx::Node& n : scene.nodes) {
            if (n.kind != "mesh") continue;
            if (!node_visible(scene, n.id)) continue;
            glm::mat4 mvp = vp * world_matrix(scene, n.id);
            const auto mc = n.components.value("mesh", nlohmann::json::object());
            std::string mesh_path = mc.value("path",    std::string(""));
            std::string tex_path  = mc.value("texture", std::string(""));
            MeshEntry& me  = get_mesh(mesh_path);
            GLuint     tex = get_tex(tex_path);
            glUniformMatrix4fv(u_mvp,1,GL_FALSE,glm::value_ptr(mvp));
            glUniform1i(u_use_tex, tex ? 1 : 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform1i(u_tex, 0);
            glBindVertexArray(me.vao);
            glDrawArrays(GL_TRIANGLES, 0, me.vert_count);
        }
        glBindVertexArray(0);

        // Wireframe collision overlay for selected visible mesh node
        if (sel_idx() >= 0) {
            const SceneObject& sel = scene_objects[sel_idx()];
            if (node_visible(scene, selected_node_id) && sel.collision != "none") {
                MeshEntry& me = get_mesh(sel.mesh_path);
                glm::mat4 wire_mvp = vp * world_matrix(scene, selected_node_id);
                glm::vec3 wcol =
                    sel.collision=="box"    ? glm::vec3{0.2f,1.0f,0.2f} :
                    sel.collision=="convex" ? glm::vec3{1.0f,0.9f,0.2f} :
                                             glm::vec3{1.0f,0.2f,0.2f};
                glDisable(GL_DEPTH_TEST);
                glUseProgram(wire_prog);
                glUniformMatrix4fv(u_wire_mvp,1,GL_FALSE,glm::value_ptr(wire_mvp));
                glUniform3fv(u_wire_col,1,glm::value_ptr(wcol));
                if (sel.collision == "mesh") {
                    glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);
                    glBindVertexArray(me.vao);
                    glDrawArrays(GL_TRIANGLES,0,me.vert_count);
                    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
                } else {
                    auto wv = make_box_wire(me.bbox_min, me.bbox_max);
                    glBindVertexArray(wire_vao);
                    glBindBuffer(GL_ARRAY_BUFFER,wire_vbo);
                    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)(wv.size()*sizeof(float)),wv.data());
                    glDrawArrays(GL_LINES,0,24);
                }
                glBindVertexArray(0);
                glEnable(GL_DEPTH_TEST);
            }
        }

        // Per-kind gizmo overlays for visible non-mesh nodes (camera, light).
        // Always rendered (independent of selection), depth-test off so they
        // remain visible through geometry.
        {
            auto draw_wire = [&](const std::vector<float>& verts,
                                 const glm::mat4& mvp,
                                 const glm::vec3& color) {
                glUniformMatrix4fv(u_wire_mvp, 1, GL_FALSE, glm::value_ptr(mvp));
                glUniform3fv(u_wire_col, 1, glm::value_ptr(color));
                glBindVertexArray(wire_vao);
                glBindBuffer(GL_ARRAY_BUFFER, wire_vbo);
                glBufferSubData(GL_ARRAY_BUFFER, 0,
                                (GLsizeiptr)(verts.size() * sizeof(float)),
                                verts.data());
                glDrawArrays(GL_LINES, 0, (GLsizei)(verts.size() / 8));
            };

            glDisable(GL_DEPTH_TEST);
            glUseProgram(wire_prog);
            for (const psx::Node& n : scene.nodes) {
                if (!node_visible(scene, n.id)) continue;
                if (n.kind == "camera") {
                    glm::mat4 mvp = vp * world_matrix(scene, n.id);
                    draw_wire(camera_wire_verts, mvp, glm::vec3(1.f, 1.f, 1.f));
                }
                else if (n.kind == "light") {
                    nlohmann::json lc = n.components.value(
                        "light", nlohmann::json::object());
                    std::string ltype = lc.value("type", std::string("directional"));
                    glm::vec3 color(1.f, 1.f, 1.f);
                    if (lc.contains("color") && lc["color"].is_array()
                        && lc["color"].size() == 3) {
                        color = { lc["color"][0].get<float>(),
                                  lc["color"][1].get<float>(),
                                  lc["color"][2].get<float>() };
                    }
                    if (ltype == "directional") {
                        glm::mat4 mvp = vp * world_matrix(scene, n.id);
                        draw_wire(dir_light_wire_verts, mvp, color);
                    } else if (ltype == "point") {
                        float radius = lc.value("radius", 1.f);
                        glm::mat4 wm = glm::scale(
                            world_matrix_no_self_scale(scene, n.id),
                            glm::vec3(radius));
                        draw_wire(point_light_wire_verts, vp * wm, color);
                    } else if (ltype == "spot") {
                        float radius = lc.value("radius", 1.f);
                        float height = lc.value("height", 2.f);
                        glm::mat4 wm = glm::scale(
                            world_matrix_no_self_scale(scene, n.id),
                            glm::vec3(radius, height, radius));
                        draw_wire(spot_light_wire_verts, vp * wm, color);
                    }
                }
            }
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }
        psx_fb.unbind();

        // ── Clear main FB ─────────────────────────────────────────────────────
        int ww,wh; SDL_GetWindowSize(win,&ww,&wh);
        glViewport(0,0,ww,wh);
        glClearColor(0.12f,0.12f,0.12f,1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        // ── ImGui ─────────────────────────────────────────────────────────────
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
#ifdef HAVE_IMGUIZMO
        ImGuizmo::BeginFrame();
#endif

        // ── Menu bar ──────────────────────────────────────────────────────────
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Project..."))
                    open_new_proj_modal_request = true;
                if (ImGui::MenuItem("Open Project..."))
                    open_open_proj_modal_request = true;
                if (ImGui::MenuItem("Close Project", nullptr, false,
                                    !current_project_path.empty()))
                    do_close_project();
                ImGui::Separator();
                if (ImGui::MenuItem("New Scene"))             do_new_scene();
                if (ImGui::MenuItem("Open Scene...","Ctrl+O"))
                    open_scene_picker_request = true;
                if (ImGui::MenuItem("Save Scene","Ctrl+S"))   do_save_scene();
                if (ImGui::MenuItem("Save Scene As...")) {
                    // Preseed only at menu-click time so a re-open from the
                    // Browse... callback doesn't clobber the picked path.
                    std::snprintf(modal_save_scene_as,
                                  sizeof(modal_save_scene_as),
                                  "%s",
                                  current_scene_path.empty()
                                      ? (current_project_path.empty()
                                             ? "scene.pscene"
                                             : "scenes/untitled.pscene")
                                      : current_scene_path.c_str());
                    open_save_scene_as_request = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) running = false;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo","Ctrl+Z",false,!undo_stack.empty()))
                    do_undo();
                if (ImGui::MenuItem("Redo","Ctrl+Y",false,!redo_stack.empty()))
                    do_redo();
                ImGui::Separator();
                if (ImGui::MenuItem("Add Object"))
                    do_add();
                if (ImGui::MenuItem("Duplicate Object","Ctrl+D",false,sel_idx()>=0))
                    do_dup();
                if (ImGui::MenuItem("Delete Object",nullptr,false,selected_node_id>0))
                    key_del = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Front",    "Num 1")) cam.snap_front();
                if (ImGui::MenuItem("Side",     "Num 3")) cam.snap_side();
                if (ImGui::MenuItem("Top",      "Num 7")) cam.snap_top();
                if (ImGui::MenuItem("Ortho/Persp","Num 5")) cam.ortho=!cam.ortho;
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Built-in file browser — renders only when open_request was set by a
        // Browse... button. Opened at top level so it's not bound to any panel.
        fs_browser_render(fs_browser);

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // ── Viewport ──────────────────────────────────────────────────────────
        ImGui::Begin("Viewport");
        {
            bool hovered = ImGui::IsWindowHovered();
            if (mmb_pressed && hovered) { orbit_active=!shift_at_press; pan_active=shift_at_press; }
            if (mmb_released)           { orbit_active=false; pan_active=false; }
            if (orbit_active) cam.orbit(mouse_dx, mouse_dy);
            if (pan_active)   cam.pan(mouse_dx, mouse_dy);
            if (hovered)      cam.zoom(scroll_ticks);
            if (key_np1) cam.snap_front();
            if (key_np3) cam.snap_side();
            if (key_np7) cam.snap_top();
            if (key_np5) cam.ortho = !cam.ortho;

            // Toolbar: gizmo orientation mode + snap settings.
            ImGui::Text("Gizmo:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::Combo("##gizmo_mode", &gizmo_mode, "Global\0Local\0");
            ImGui::SameLine();
            ImGui::Checkbox("Snap", &snap_enabled);
            ImGui::SameLine();
            if (ImGui::SmallButton("...##snap_settings"))
                ImGui::OpenPopup("snap_settings_popup");
            if (ImGui::BeginPopup("snap_settings_popup")) {
                ImGui::TextUnformatted("Snap settings");
                ImGui::Separator();
                ImGui::DragFloat("Translate",    &snap_translate,
                                 0.05f,  0.001f, 10.f, "%.3f");
                ImGui::DragFloat("Rotate (deg)", &snap_rotate,
                                 0.5f,   0.1f,   90.f, "%.2f");
                ImGui::DragFloat("Scale",        &snap_scale,
                                 0.005f, 0.001f, 1.f,  "%.3f");
                if (ImGui::Button("Reset to defaults")) {
                    snap_translate = 0.5f;
                    snap_rotate    = 15.f;
                    snap_scale     = 0.1f;
                }
                ImGui::EndPopup();
            }
            ImGui::TextDisabled("[T] Translate  [R] Rotate  [S] Scale  |  "
                                "Ctrl=Snap  Shift=Precision");

            ImVec2 avail = ImGui::GetContentRegionAvail();
            float fw = avail.x, fh = avail.y;
            if (fh>0.f && fw/fh > 4.f/3.f) fw = fh*(4.f/3.f);
            else if (fw>0.f)                fh = fw*(3.f/4.f);
            ImVec2 cur = ImGui::GetCursorPos();
            ImGui::SetCursorPos({cur.x+(avail.x-fw)*0.5f, cur.y+(avail.y-fh)*0.5f});
            ImVec2 img_screen = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(intptr_t)psx_fb.tex, {fw,fh}, {0,1},{1,0});

#ifdef HAVE_IMGUIZMO
            if (selected_node_id >= 0) {
                if (psx::Node* sel_node = scene.find(selected_node_id)) {
                    glm::mat4 view_mat = cam.view();
                    glm::mat4 proj_mat = cam.proj(fw/(fh>0?fh:1.f));
                    // Gizmo manipulates the WORLD transform.
                    glm::mat4 model = world_matrix(scene, selected_node_id);

                    ImGuizmo::SetOrthographic(cam.ortho);
                    ImGuizmo::SetDrawlist();
                    ImGuizmo::SetRect(img_screen.x, img_screen.y, fw, fh);
                    static const ImGuizmo::OPERATION ops[] = {
                        ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE
                    };
                    // ImGuizmo: WORLD = global axes, LOCAL = aligned to object.
                    ImGuizmo::MODE imode = (gizmo_mode == GIZMO_LOCAL)
                                           ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

                    // Snap (toggle OR Ctrl) and precision (Shift) modifiers.
                    SDL_Keymod kmod  = SDL_GetModState();
                    bool ctrl_held   = (kmod & KMOD_CTRL)  != 0;
                    bool shift_held  = (kmod & KMOD_SHIFT) != 0;
                    bool snap_now    = snap_enabled || ctrl_held;
                    float snap_arr[3] = {0.f, 0.f, 0.f};
                    if (snap_now) {
                        float v = (gizmo_op == 0) ? snap_translate
                                : (gizmo_op == 1) ? snap_rotate
                                                  : snap_scale;
                        snap_arr[0] = snap_arr[1] = snap_arr[2] = v;
                    }

                    glm::mat4 prev_model = model;
                    ImGuizmo::Manipulate(
                        glm::value_ptr(view_mat), glm::value_ptr(proj_mat),
                        ops[gizmo_op], imode, glm::value_ptr(model),
                        nullptr,
                        snap_now ? snap_arr : nullptr);

                    bool gizmo_using_now = ImGuizmo::IsUsing();
                    if (gizmo_using_now && !gizmo_was_using) push_undo();
                    if (gizmo_using_now) {
                        // Precision: blend manipulated → 10% of frame delta.
                        if (shift_held) {
                            glm::vec3 t_prev, t_new, s_prev, s_new;
                            glm::quat r_prev, r_new;
                            decompose_trs_q(prev_model, t_prev, r_prev, s_prev);
                            decompose_trs_q(model,       t_new,  r_new,  s_new);
                            glm::vec3 t = t_prev + (t_new - t_prev) * 0.1f;
                            glm::quat r = glm::slerp(r_prev, r_new, 0.1f);
                            glm::vec3 s = s_prev + (s_new - s_prev) * 0.1f;
                            model = glm::translate(glm::mat4(1.f), t)
                                  * glm::mat4_cast(r);
                            model = glm::scale(model, s);
                        }
                        // Convert the new world matrix back to a local one.
                        glm::mat4 pw = parent_world(scene, selected_node_id);
                        glm::mat4 new_local = glm::inverse(pw) * model;
                        float t[3], r[3], s[3];
                        ImGuizmo::DecomposeMatrixToComponents(
                            glm::value_ptr(new_local), t, r, s);
                        glm::vec3 euler = {r[0], r[1], r[2]};
                        sel_node->position = {t[0], t[1], t[2]};
                        sel_node->rotation = glm::quat(glm::radians(euler));
                        sel_node->scale    = {s[0], s[1], s[2]};
                        // Keep the Inspector's preferred-Euler in step.
                        preferred_euler_deg[selected_node_id] = euler;
                        int mi = mesh_index_for_node(selected_node_id);
                        if (mi >= 0 && mi < (int)scene_objects.size()) {
                            scene_objects[mi].position = sel_node->position;
                            scene_objects[mi].rotation = euler;
                            scene_objects[mi].scale    = sel_node->scale;
                        }
                        scene_dirty  = true;
                    }
                    if (shift_held && gizmo_using_now) {
                        ImVec2 corner = ImGui::GetWindowPos();
                        ImVec2 sz = ImGui::GetWindowSize();
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(corner.x + 10.f,
                                   corner.y + sz.y - 26.f),
                            IM_COL32(255, 220, 100, 255),
                            "Precision");
                    }
                    gizmo_was_using = gizmo_using_now;
                }
            }
#endif
        }
        ImGui::End();

        // ── Outliner ──────────────────────────────────────────────────────────
        ImGui::Begin("Outliner");
        if (ImGui::Button("+")) ImGui::OpenPopup("add_object_popup");
        if (ImGui::BeginPopup("add_object_popup")) {
            // Empty Node ───────────────────────────────────────────────────
            if (ImGui::MenuItem("Empty Node")) {
                push_undo();
                psx::Node n;
                n.name = "Empty";
                n.kind = "node";
                selected_node_id = add_under_selected(std::move(n));
                sync_objects_from_scene();
                scene_dirty = true;
            }
            // Mesh ─────────────────────────────────────────────────────────
            if (ImGui::BeginMenu("Mesh")) {
                struct { const char* label; const char* path; } items[] = {
                    {"From File", "assets/meshes/test.obj"},
                    {"Cube",      "__primitive_cube__"},
                    {"Plane",     "__primitive_plane__"},
                    {"Sphere",    "__primitive_sphere__"},
                    {"Cylinder",  "__primitive_cylinder__"},
                    {"Cone",      "__primitive_cone__"},
                    {"Capsule",   "__primitive_capsule__"},
                };
                for (auto& it : items) {
                    if (ImGui::MenuItem(it.label)) {
                        push_undo();
                        psx::Node n;
                        n.name = it.label;
                        n.kind = "mesh";
                        n.components["mesh"]["path"]      = it.path;
                        n.components["mesh"]["texture"]   =
                            std::string(it.path) == "assets/meshes/test.obj"
                            ? "assets/textures/test.tga" : "";
                        n.components["collision"]["type"] = "box";
                        selected_node_id = add_under_selected(std::move(n));
                        sync_objects_from_scene();
                        scene_dirty = true;
                    }
                }
                ImGui::EndMenu();
            }
            // Camera ───────────────────────────────────────────────────────
            if (ImGui::MenuItem("Camera")) {
                push_undo();
                psx::Node n;
                n.name = "Camera";
                n.kind = "camera";
                n.components["camera"]["fov"]  = 60;
                n.components["camera"]["near"] = 0.1;
                n.components["camera"]["far"]  = 100;
                selected_node_id = add_under_selected(std::move(n));
                sync_objects_from_scene();
                scene_dirty = true;
            }
            // Light ────────────────────────────────────────────────────────
            if (ImGui::BeginMenu("Light")) {
                struct { const char* label; const char* type; } lights[] = {
                    {"Directional", "directional"},
                    {"Point",       "point"},
                    {"Spot",        "spot"},
                };
                for (auto& l : lights) {
                    if (ImGui::MenuItem(l.label)) {
                        push_undo();
                        psx::Node n;
                        n.name = l.label;
                        n.kind = "light";
                        n.components["light"]["type"]      = l.type;
                        n.components["light"]["color"]     = {1.0, 1.0, 1.0};
                        n.components["light"]["intensity"] = 1.0;
                        n.components["light"]["radius"]    = 1.0;
                        n.components["light"]["height"]    = 2.0;
                        selected_node_id = add_under_selected(std::move(n));
                        sync_objects_from_scene();
                        scene_dirty = true;
                    }
                }
                ImGui::EndMenu();
            }
            // Player (with auto-created Camera + Capsule children) ─────────
            if (ImGui::MenuItem("Player")) {
                push_undo();
                psx::Node player;
                player.name = "Player";
                player.kind = "player";
                player.components["spawn"]["is_default"] = true;
                int player_id = add_under_selected(std::move(player));

                psx::Node cam_node;
                cam_node.name     = "Camera";
                cam_node.kind     = "camera";
                cam_node.parent   = player_id;
                cam_node.position = {0.f, 1.5f, 0.f};
                cam_node.components["camera"]["fov"]  = 60;
                cam_node.components["camera"]["near"] = 0.1;
                cam_node.components["camera"]["far"]  = 100;
                scene.add_node(std::move(cam_node));

                psx::Node cap;
                cap.name     = "Capsule";
                cap.kind     = "mesh";
                cap.parent   = player_id;
                cap.scale    = {0.5f, 1.f, 0.5f};
                cap.components["mesh"]["path"]      = "__primitive_capsule__";
                cap.components["mesh"]["texture"]   = "";
                cap.components["collision"]["type"] = "convex";
                scene.add_node(std::move(cap));

                selected_node_id = player_id;
                sync_objects_from_scene();
                scene_dirty = true;
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();

        // Recursive tree walk over scene.nodes, rooted at parent==-1.
        std::function<void(int)> draw_node = [&](int node_id) {
            psx::Node* n = scene.find(node_id);
            if (!n) return;

            ImGui::PushID(node_id);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.1f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.2f));
            if (ImGui::SmallButton(n->visible ? "[v]" : "[ ]")) {
                push_undo();
                n->visible = !n->visible;
                int mi = mesh_index_for_node(node_id);
                if (mi >= 0 && mi < (int)scene_objects.size())
                    scene_objects[mi].visible = n->visible;
                scene_dirty = true;
            }
            ImGui::PopStyleColor(3);
            ImGui::PopID();
            ImGui::SameLine();

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_SpanAvailWidth
                                     | ImGuiTreeNodeFlags_DefaultOpen;
            if (n->children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
            if (selected_node_id == node_id) flags |= ImGuiTreeNodeFlags_Selected;

            bool open = ImGui::TreeNodeEx((void*)(intptr_t)node_id, flags,
                                          "%s", n->name.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                selected_node_id = (selected_node_id == node_id) ? -1 : node_id;

            // Drag source: payload = this node's id.
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("PSX_NODE_ID", &node_id, sizeof(int));
                ImGui::Text("%s", n->name.c_str());
                ImGui::EndDragDropSource();
            }
            // Drop target: reparent dragged node onto this row.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("PSX_NODE_ID")) {
                    int dragged_id = *(const int*)payload->Data;
                    reparent(dragged_id, node_id);
                }
                ImGui::EndDragDropTarget();
            }

            if (open) {
                // Snapshot children — recursive callbacks may mutate the tree.
                std::vector<int> kids = n->children;
                for (int cid : kids) draw_node(cid);
                ImGui::TreePop();
            }
        };

        for (const psx::Node& root_n : scene.nodes)
            if (root_n.parent == -1) draw_node(root_n.id);

        if (!status_msg.empty() && SDL_GetTicks() < status_until)
            ImGui::TextDisabled("%s", status_msg.c_str());

        // Empty area below the tree → drop here to reparent to the scene root.
        {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x > 0.f && avail.y > 0.f) {
                ImGui::InvisibleButton("##outliner_root_drop", avail);
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload("PSX_NODE_ID")) {
                        int dragged_id = *(const int*)payload->Data;
                        reparent(dragged_id, 0);
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        }
        ImGui::End();

        // Deferred delete: any non-root node + its descendant subtree.
        if (key_del && selected_node_id > 0 && scene.find(selected_node_id)) {
            push_undo();
            int del_root = selected_node_id;

            // Collect del_root + all descendants.
            std::vector<int> to_delete;
            std::function<void(int)> collect = [&](int id) {
                psx::Node* nn = scene.find(id);
                if (!nn) return;
                to_delete.push_back(id);
                std::vector<int> kids = nn->children;
                for (int cid : kids) collect(cid);
            };
            collect(del_root);

            // Detach del_root from its parent's children list.
            for (auto& other : scene.nodes) {
                auto cit = std::find(other.children.begin(),
                                     other.children.end(), del_root);
                if (cit != other.children.end()) other.children.erase(cit);
            }

            // Erase all collected nodes from scene.nodes.
            auto rit = std::remove_if(scene.nodes.begin(), scene.nodes.end(),
                [&](const psx::Node& n) {
                    return std::find(to_delete.begin(), to_delete.end(), n.id)
                           != to_delete.end();
                });
            scene.nodes.erase(rit, scene.nodes.end());

            // Drop preferred-Euler entries for everything we just removed.
            for (int id : to_delete) preferred_euler_deg.erase(id);

            selected_node_id = -1;
            sync_objects_from_scene();
            scene_dirty = true;
        }

        // ── Inspector ─────────────────────────────────────────────────────────
        ImGui::Begin("Inspector");
        if (psx::Node* node = (selected_node_id >= 0)
                              ? scene.find(selected_node_id) : nullptr) {
            const int mi = mesh_index_for_node(selected_node_id);

            // Name + kind badge ───────────────────────────────────────────
            {
                char name_buf[256];
                std::snprintf(name_buf, sizeof(name_buf), "%s", node->name.c_str());
                if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
                    node->name = name_buf;
                    if (mi >= 0 && mi < (int)scene_objects.size())
                        scene_objects[mi].name = name_buf;
                    scene_dirty = true;
                }
                if (ImGui::IsItemActivated()) push_undo();
                ImGui::SameLine();
                ImGui::TextDisabled("[%s]", node->kind.c_str());
            }

            // Transform (every kind) ───────────────────────────────────────
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat3("Position", &node->position.x, 0.1f);
                if (ImGui::IsItemActivated()) push_undo();
                if (ImGui::IsItemEdited()) {
                    if (mi >= 0 && mi < (int)scene_objects.size())
                        scene_objects[mi].position = node->position;
                    scene_dirty = true;
                }
                // Rotation is stored as a quaternion on the node, but the
                // Inspector edits Euler XYZ in degrees. Hold a stable Euler
                // per-node so the triple doesn't jump as the underlying quat
                // is decomposed.
                auto pit = preferred_euler_deg.find(node->id);
                if (pit == preferred_euler_deg.end()) {
                    glm::vec3 e = glm::degrees(glm::eulerAngles(node->rotation));
                    pit = preferred_euler_deg.emplace(node->id, e).first;
                }
                glm::vec3& euler_deg = pit->second;
                ImGui::DragFloat3("Rotation", &euler_deg.x, 1.0f);
                if (ImGui::IsItemActivated()) push_undo();
                if (ImGui::IsItemEdited()) {
                    node->rotation = glm::quat(glm::radians(euler_deg));
                    if (mi >= 0 && mi < (int)scene_objects.size())
                        scene_objects[mi].rotation = euler_deg;
                    scene_dirty = true;
                }
                ImGui::DragFloat3("Scale", &node->scale.x, 0.01f, 0.001f, FLT_MAX, "%.3f");
                if (ImGui::IsItemActivated()) push_undo();
                if (ImGui::IsItemEdited()) {
                    if (mi >= 0 && mi < (int)scene_objects.size())
                        scene_objects[mi].scale = node->scale;
                    scene_dirty = true;
                }
            }

            // Helper: ensure a json subobject exists, return reference.
            auto ensure_obj = [&](const char* key) -> nlohmann::json& {
                auto& slot = node->components[key];
                if (!slot.is_object()) slot = nlohmann::json::object();
                return slot;
            };

            // ── kind == "mesh" ────────────────────────────────────────────
            if (node->kind == "mesh" && mi >= 0
                && mi < (int)scene_objects.size())
            {
                SceneObject& obj = scene_objects[mi];
                auto path_field = [&](const char* label, std::string& path,
                                      const char* component_key,
                                      const char* field_key,
                                      const char* dir,
                                      std::initializer_list<const char*> exts,
                                      const char* fid) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("%s", label); ImGui::SameLine();
                    char buf[512]; std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 35.f);
                    char in_id[32], btn_id[32];
                    std::snprintf(in_id,  sizeof(in_id),  "##%s_in", fid);
                    std::snprintf(btn_id, sizeof(btn_id), "...##%s", fid);
                    if (ImGui::InputText(in_id, buf, sizeof(buf))) {
                        path = buf;
                        ensure_obj(component_key)[field_key] = buf;
                        scene_dirty = true;
                    }
                    if (ImGui::IsItemActivated()) push_undo();
                    ImGui::SameLine();
                    if (ImGui::Button(btn_id)) picker.open(&path, dir, exts);
                };

                if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
                    const std::string mesh_dir = assets_dir("meshes");
                    const std::string tex_dir  = assets_dir("textures");
                    path_field("Path",    obj.mesh_path,    "mesh", "path",
                               mesh_dir.c_str(), {".obj"},                "mesh");
                    auto wit = validation_warnings.find(obj.mesh_path);
                    if (wit != validation_warnings.end())
                        for (const auto& w : wit->second)
                            ImGui::TextColored({1.f, 0.85f, 0.f, 1.f}, "! %s", w.c_str());
                    path_field("Texture", obj.texture_path, "mesh", "texture",
                               tex_dir.c_str(), {".tga",".png",".bmp"}, "tex");
                }
                if (ImGui::CollapsingHeader("Collision", ImGuiTreeNodeFlags_DefaultOpen)) {
                    const char* coll_items[] = {"none","box","mesh","convex"};
                    int coll_idx = 0;
                    for (int ci = 0; ci < 4; ++ci)
                        if (obj.collision == coll_items[ci]) { coll_idx = ci; break; }
                    if (ImGui::Combo("Type", &coll_idx, coll_items, 4)) {
                        push_undo();
                        obj.collision = coll_items[coll_idx];
                        ensure_obj("collision")["type"] = obj.collision;
                        scene_dirty = true;
                    }
                }
            }
            // ── kind == "camera" ──────────────────────────────────────────
            else if (node->kind == "camera") {
                if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& cam_c = ensure_obj("camera");

                    float fov = cam_c.value("fov", 60.f);
                    if (ImGui::DragFloat("FOV", &fov, 0.5f, 1.f, 179.f, "%.1f")) {
                        cam_c["fov"] = fov; scene_dirty = true;
                    }
                    if (ImGui::IsItemActivated()) push_undo();

                    float near_p = cam_c.value("near", 0.1f);
                    if (ImGui::DragFloat("Near", &near_p, 0.01f, 0.01f, 10.f, "%.2f")) {
                        cam_c["near"] = near_p; scene_dirty = true;
                    }
                    if (ImGui::IsItemActivated()) push_undo();

                    float far_p = cam_c.value("far", 100.f);
                    if (ImGui::DragFloat("Far", &far_p, 0.5f, 1.f, 1000.f, "%.1f")) {
                        cam_c["far"] = far_p; scene_dirty = true;
                    }
                    if (ImGui::IsItemActivated()) push_undo();

                    bool active = cam_c.value("is_active", false);
                    if (ImGui::Checkbox("Active camera", &active)) {
                        push_undo();
                        if (active) {
                            for (auto& other : scene.nodes) {
                                if (other.kind != "camera" || other.id == node->id)
                                    continue;
                                auto& oc = other.components["camera"];
                                if (!oc.is_object()) oc = nlohmann::json::object();
                                oc["is_active"] = false;
                            }
                        }
                        cam_c["is_active"] = active;
                        scene_dirty = true;
                    }
                }
            }
            // ── kind == "light" ───────────────────────────────────────────
            else if (node->kind == "light") {
                if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& lc = ensure_obj("light");

                    const char* type_items[] = {"directional","point","spot"};
                    std::string current_type =
                        lc.value("type", std::string("directional"));
                    int type_idx = 0;
                    for (int ti = 0; ti < 3; ++ti)
                        if (current_type == type_items[ti]) { type_idx = ti; break; }
                    if (ImGui::Combo("Type", &type_idx, type_items, 3)) {
                        push_undo();
                        lc["type"] = type_items[type_idx];
                        current_type = type_items[type_idx];
                        scene_dirty = true;
                    }

                    float color[3] = {1.f, 1.f, 1.f};
                    if (lc.contains("color") && lc["color"].is_array()
                        && lc["color"].size() == 3) {
                        color[0] = lc["color"][0].get<float>();
                        color[1] = lc["color"][1].get<float>();
                        color[2] = lc["color"][2].get<float>();
                    }
                    if (ImGui::ColorEdit3("Color", color)) {
                        lc["color"] = {color[0], color[1], color[2]};
                        scene_dirty = true;
                    }
                    if (ImGui::IsItemActivated()) push_undo();

                    float intensity = lc.value("intensity", 1.f);
                    if (ImGui::DragFloat("Intensity", &intensity, 0.05f, 0.f, 10.f, "%.2f")) {
                        lc["intensity"] = intensity;
                        scene_dirty = true;
                    }
                    if (ImGui::IsItemActivated()) push_undo();

                    if (current_type == "point" || current_type == "spot") {
                        float radius = lc.value("radius", 1.f);
                        if (ImGui::DragFloat("Radius", &radius, 0.1f, 0.1f, 50.f, "%.2f")) {
                            lc["radius"] = radius;
                            scene_dirty = true;
                        }
                        if (ImGui::IsItemActivated()) push_undo();
                    }
                    if (current_type == "spot") {
                        float height = lc.value("height", 2.f);
                        if (ImGui::DragFloat("Height", &height, 0.1f, 0.1f, 50.f, "%.2f")) {
                            lc["height"] = height;
                            scene_dirty = true;
                        }
                        if (ImGui::IsItemActivated()) push_undo();
                    }
                }
            }
            // ── kind == "player" ──────────────────────────────────────────
            else if (node->kind == "player") {
                if (ImGui::CollapsingHeader("Spawn", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& sc = ensure_obj("spawn");
                    bool is_default = sc.value("is_default", false);
                    if (ImGui::Checkbox("Default spawn", &is_default)) {
                        push_undo();
                        if (is_default) {
                            for (auto& other : scene.nodes) {
                                if (other.kind != "player" || other.id == node->id)
                                    continue;
                                auto& os = other.components["spawn"];
                                if (!os.is_object()) os = nlohmann::json::object();
                                os["is_default"] = false;
                            }
                        }
                        sc["is_default"] = is_default;
                        scene_dirty = true;
                    }
                }
                if (ImGui::CollapsingHeader("Controller", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& cc = ensure_obj("controller");

                    const char* type_items[] = {"fps","orbit","scripted"};
                    std::string current_type = cc.value("type", std::string("fps"));
                    int type_idx = 0;
                    for (int ci = 0; ci < 3; ++ci)
                        if (current_type == type_items[ci]) { type_idx = ci; break; }
                    if (ImGui::Combo("Type", &type_idx, type_items, 3)) {
                        push_undo();
                        cc["type"] = type_items[type_idx];
                        scene_dirty = true;
                    }

                    float speed = cc.value("speed", 5.f);
                    if (ImGui::DragFloat("Speed", &speed, 0.1f, 0.f, 100.f, "%.2f")) {
                        cc["speed"] = speed; scene_dirty = true;
                    }
                    if (ImGui::IsItemActivated()) push_undo();

                    float sens = cc.value("mouse_sensitivity", 0.1f);
                    if (ImGui::DragFloat("Mouse sensitivity", &sens,
                                         0.005f, 0.f, 5.f, "%.3f")) {
                        cc["mouse_sensitivity"] = sens; scene_dirty = true;
                    }
                    if (ImGui::IsItemActivated()) push_undo();
                }
            }
            // kind == "node" → name + transform only, no extra sections.

#ifdef HAVE_IMGUIZMO
            ImGui::Separator();
            ImGui::TextDisabled("T=translate  R=rotate  S=scale");
#endif
        } else {
            ImGui::TextDisabled("No object selected");
        }

        // File picker modal — paths picked while a project is open are
        // converted to project-relative form so .pscene files remain portable
        // across machines that have the project at different absolute paths.
        if (ImGui::BeginPopupModal("File Picker", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Select file:"); ImGui::Separator();
            if (picker.files.empty()) {
                ImGui::TextDisabled("No files found");
            } else {
                for (const auto& fp : picker.files) {
                    if (ImGui::Selectable(fp.c_str())) {
                        push_undo();
                        const std::string stored = rel_to_project(fp);
                        if (picker.dest) *picker.dest = stored;
                        picker.dest  = nullptr;
                        if (psx::Node* nn = scene.find(selected_node_id);
                            nn && sel_idx() >= 0) {
                            const SceneObject& obj = scene_objects[sel_idx()];
                            nn->components["mesh"]["path"]    = obj.mesh_path;
                            nn->components["mesh"]["texture"] = obj.texture_path;
                        }
                        scene_dirty  = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::Button("Cancel", {120,0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ── New Project modal ─────────────────────────────────────────────────
        if (open_new_proj_modal_request) {
            ImGui::OpenPopup("New Project");
            open_new_proj_modal_request = false;
        }
        if (ImGui::BeginPopupModal("New Project", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Parent directory:");
            ImGui::SetNextItemWidth(420);
            ImGui::InputText("##np_parent", modal_new_proj_parent,
                             sizeof(modal_new_proj_parent));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##np_browse")) {
                // External tool first if user has $PSX_FILE_PICKER configured;
                // otherwise the built-in ImGui browser. The external path is
                // synchronous, so we update + stay in this modal in one go.
                std::string ext = run_external_picker();
                if (!ext.empty()) {
                    std::snprintf(modal_new_proj_parent,
                                  sizeof(modal_new_proj_parent),
                                  "%s", ext.c_str());
                } else if (!std::getenv("PSX_FILE_PICKER")) {
                    namespace fs = std::filesystem;
                    fs_browser.mode  = FsBrowser::OPEN_DIR;
                    fs_browser.title = "Select parent directory for new project";
                    fs_browser.cwd   = (modal_new_proj_parent[0] &&
                                        fs::is_directory(modal_new_proj_parent))
                        ? std::string(modal_new_proj_parent)
                        : fs::current_path().string();
                    fs_browser.filename[0] = '\0';
                    fs_browser.exts.clear();
                    fs_browser.on_accept = [&](const std::string& picked) {
                        std::snprintf(modal_new_proj_parent,
                                      sizeof(modal_new_proj_parent),
                                      "%s", picked.c_str());
                        open_new_proj_modal_request = true;
                    };
                    fs_browser.open_request = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Text("Project name:");
            ImGui::SetNextItemWidth(420);
            ImGui::InputText("##np_name", modal_new_proj_name,
                             sizeof(modal_new_proj_name));
            ImGui::Separator();
            const bool can_create = modal_new_proj_parent[0] != '\0'
                                 && modal_new_proj_name[0]   != '\0';
            ImGui::BeginDisabled(!can_create);
            if (ImGui::Button("Create", {120, 0})) {
                if (do_new_project(modal_new_proj_parent, modal_new_proj_name))
                    ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ── Open Project modal ────────────────────────────────────────────────
        if (open_open_proj_modal_request) {
            ImGui::OpenPopup("Open Project");
            open_open_proj_modal_request = false;
        }
        if (ImGui::BeginPopupModal("Open Project", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Path to project.psxproj:");
            ImGui::SetNextItemWidth(420);
            ImGui::InputText("##op_path", modal_open_proj_path,
                             sizeof(modal_open_proj_path));
            ImGui::Separator();
            ImGui::BeginDisabled(modal_open_proj_path[0] == '\0');
            if (ImGui::Button("Open", {120, 0})) {
                if (do_open_project(modal_open_proj_path))
                    ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ── Open Scene picker ─────────────────────────────────────────────────
        if (open_scene_picker_request) {
            namespace fs = std::filesystem;
            scene_picker_files.clear();
            const std::string dir = current_project_path.empty()
                ? std::string(".")
                : (project_root_dir() + "/scenes");
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                for (char& c : ext) c = (char)tolower((unsigned char)c);
                if (ext == ".pscene")
                    scene_picker_files.push_back(entry.path().string());
            }
            std::sort(scene_picker_files.begin(), scene_picker_files.end());
            ImGui::OpenPopup("Open Scene");
            open_scene_picker_request = false;
        }
        if (ImGui::BeginPopupModal("Open Scene", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Select scene:"); ImGui::Separator();
            if (scene_picker_files.empty()) {
                ImGui::TextDisabled("No .pscene files found");
            } else {
                for (const auto& fp : scene_picker_files) {
                    if (ImGui::Selectable(fp.c_str())) {
                        const std::string stored = current_project_path.empty()
                            ? fp : rel_to_project(fp);
                        do_load_scene_from(stored);
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ── Save Scene As modal ───────────────────────────────────────────────
        // The buffer is preseeded at menu-click time and by the Browse...
        // callback — never reseeded here, so re-opening keeps the latest value.
        if (open_save_scene_as_request) {
            ImGui::OpenPopup("Save Scene As");
            open_save_scene_as_request = false;
        }
        if (ImGui::BeginPopupModal("Save Scene As", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            if (current_project_path.empty())
                ImGui::TextDisabled("Path (free-floating mode):");
            else
                ImGui::TextDisabled("Path (relative to project root):");
            ImGui::SetNextItemWidth(420);
            ImGui::InputText("##sa_path", modal_save_scene_as,
                             sizeof(modal_save_scene_as));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##sa_browse")) {
                // External tool first ($PSX_FILE_PICKER); built-in fallback.
                auto store_picked = [&](std::string picked) {
                    if (picked.empty()) return;
                    if (std::filesystem::path(picked).extension().empty())
                        picked += ".pscene";
                    std::string stored = current_project_path.empty()
                        ? picked : rel_to_project(picked);
                    std::snprintf(modal_save_scene_as,
                                  sizeof(modal_save_scene_as),
                                  "%s", stored.c_str());
                };
                std::string ext = run_external_picker();
                if (!ext.empty()) {
                    store_picked(ext);
                } else if (!std::getenv("PSX_FILE_PICKER")) {
                    namespace fs = std::filesystem;
                    // Seed dir at <project>/scenes when project is open,
                    // otherwise at the dir of the typed path or cwd.
                    std::string seed_dir;
                    std::string seed_name;
                    fs::path typed(modal_save_scene_as);
                    if (!current_project_path.empty()) {
                        seed_dir = typed.is_absolute()
                            ? typed.parent_path().string()
                            : (fs::path(project_root_dir()) /
                               (typed.has_parent_path()
                                    ? typed.parent_path()
                                    : fs::path("scenes"))).string();
                    } else {
                        seed_dir = typed.has_parent_path()
                            ? typed.parent_path().string()
                            : fs::current_path().string();
                    }
                    if (!fs::is_directory(seed_dir))
                        seed_dir = fs::current_path().string();
                    seed_name = typed.filename().string();

                    fs_browser.mode  = FsBrowser::SAVE_FILE;
                    fs_browser.title = "Save scene as";
                    fs_browser.cwd   = seed_dir;
                    std::snprintf(fs_browser.filename,
                                  sizeof(fs_browser.filename),
                                  "%s", seed_name.c_str());
                    fs_browser.exts = {".pscene"};
                    fs_browser.on_accept =
                        [&, store_picked](const std::string& picked) {
                            store_picked(picked);
                            open_save_scene_as_request = true;
                        };
                    fs_browser.open_request = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Separator();
            ImGui::BeginDisabled(modal_save_scene_as[0] == '\0');
            if (ImGui::Button("Save", {120, 0})) {
                do_save_scene_to(modal_save_scene_as);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::End();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (orbit_active)    SDL_SetCursor(cur_crosshair);
        else if (pan_active) SDL_SetCursor(cur_sizeall);
        else                 SDL_SetCursor(cur_arrow);

        // ── Window title ──────────────────────────────────────────────────────
        {
            std::string scene_label = current_scene_path.empty()
                ? std::string("untitled")
                : std::filesystem::path(current_scene_path).filename().string();
            std::string title = "psx-editor \xe2\x80\x94 ";
            if (!current_project_path.empty())
                title += current_project.name + " / " + scene_label;
            else
                title += scene_label;
            title += (scene_dirty ? " *" : "");
            if (title != last_title) {
                SDL_SetWindowTitle(win, title.c_str());
                last_title = title;
            }
        }

        SDL_GL_SwapWindow(win);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    psx_fb.destroy();
    glDeleteProgram(mesh_prog);
    glDeleteProgram(wire_prog);
    glDeleteVertexArrays(1,&wire_vao);
    glDeleteBuffers(1,&wire_vbo);
    for (auto& [k,me] : mesh_cache) {
        if (me.vao) glDeleteVertexArrays(1,&me.vao);
        if (me.vbo) glDeleteBuffers(1,&me.vbo);
    }
    for (auto& [k,tex] : tex_cache) if (tex) glDeleteTextures(1,&tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_FreeCursor(cur_arrow);
    SDL_FreeCursor(cur_crosshair);
    SDL_FreeCursor(cur_sizeall);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
