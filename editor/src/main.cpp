// psx-editor — milestone 3 + built-in primitive meshes
#include <cstdio>
#include <cstdint>
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

#include <nlohmann/json.hpp>

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
#include <glm/gtc/type_ptr.hpp>

// ── Helpers ───────────────────────────────────────────────────────────────────
static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return ""; }
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
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

// ── Scene I/O ─────────────────────────────────────────────────────────────────
static void save_scene(const std::vector<SceneObject>& objects,
                       const char* path, std::string& status) {
    using json = nlohmann::json;
    json root; root["name"] = "untitled"; root["objects"] = json::array();
    for (const auto& obj : objects) {
        json o;
        o["name"]         = obj.name;
        o["mesh_path"]    = obj.mesh_path;
        o["texture_path"] = obj.texture_path;
        o["position"]     = {obj.position.x, obj.position.y, obj.position.z};
        o["rotation"]     = {obj.rotation.x, obj.rotation.y, obj.rotation.z};
        o["scale"]        = {obj.scale.x,    obj.scale.y,    obj.scale.z};
        o["visible"]      = obj.visible;
        o["collision"]    = obj.collision;
        root["objects"].push_back(o);
    }
    std::ofstream f(path);
    if (!f) { status = "Error."; return; }
    f << root.dump(2);
    status = "Saved.";
    fprintf(stderr, "scene: saved %zu objects -> %s\n", objects.size(), path);
}

static void load_scene(std::vector<SceneObject>& objects, int& selected_index,
                       const char* path, std::string& status) {
    using json = nlohmann::json;
    std::ifstream f(path);
    if (!f) { status = "Error."; return; }
    json root;
    try { root = json::parse(f); } catch (...) { status = "Error."; return; }
    objects.clear();
    for (const auto& o : root.value("objects", json::array())) {
        SceneObject obj;
        obj.name         = o.value("name",         "Object");
        obj.mesh_path    = o.value("mesh_path",    "assets/meshes/test.obj");
        obj.texture_path = o.value("texture_path", "assets/textures/test.tga");
        obj.visible      = o.value("visible",      true);
        obj.collision    = o.value("collision",    "box");
        auto get3 = [&](const char* key, glm::vec3 def) -> glm::vec3 {
            auto v = o.value(key, std::vector<float>{def.x,def.y,def.z});
            return v.size()==3 ? glm::vec3{v[0],v[1],v[2]} : def;
        };
        obj.position = get3("position",{0,0,0});
        obj.rotation = get3("rotation",{0,0,0});
        obj.scale    = get3("scale",   {1,1,1});
        objects.push_back(std::move(obj));
    }
    selected_index = -1;
    status = "Loaded.";
    fprintf(stderr, "scene: loaded %zu objects <- %s\n", objects.size(), path);
}

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

    GLuint wire_vao=0, wire_vbo=0;
    glGenVertexArrays(1,&wire_vao); glGenBuffers(1,&wire_vbo);
    glBindVertexArray(wire_vao);
    glBindBuffer(GL_ARRAY_BUFFER,wire_vbo);
    glBufferData(GL_ARRAY_BUFFER,24*8*sizeof(float),nullptr,GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)0);
    glBindVertexArray(0);

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
        };
        for (auto& p : prims)
            mesh_cache[p.key] = upload_mesh(generate_primitive(p.type));
    }

    auto get_mesh = [&](const std::string& path) -> MeshEntry& {
        auto it = mesh_cache.find(path);
        if (it != mesh_cache.end()) return it->second;
        LoadedMesh lm = load_obj(path.c_str());
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

    auto get_tex = [&](const std::string& path) -> GLuint {
        auto it = tex_cache.find(path);
        if (it != tex_cache.end()) return it->second;
        TGAImage img = load_tga(path.c_str());
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
    std::vector<SceneObject> scene_objects = {{"Cube"},{"Light"},{"Spawn Point"}};
    int         selected_index = -1;
    std::string status_msg;
    Uint32      status_until  = 0;
    FilePicker  picker;
    int         gizmo_op      = 0;      // 0=translate 1=rotate 2=scale
    std::string current_file;           // empty = untitled
    bool        scene_dirty   = false;
    std::string last_title;

    // ── Undo / redo ───────────────────────────────────────────────────────────
    struct Snapshot { std::vector<SceneObject> objects; int selected; };
    static constexpr int MAX_UNDO = 50;
    std::vector<Snapshot> undo_stack, redo_stack;

    auto push_undo = [&]() {
        undo_stack.push_back({scene_objects, selected_index});
        if ((int)undo_stack.size() > MAX_UNDO)
            undo_stack.erase(undo_stack.begin());
        redo_stack.clear();
    };

    auto do_undo = [&]() {
        if (undo_stack.empty()) return;
        redo_stack.push_back({scene_objects, selected_index});
        scene_objects  = undo_stack.back().objects;
        selected_index = undo_stack.back().selected;
        undo_stack.pop_back();
        scene_dirty = true;
    };

    auto do_redo = [&]() {
        if (redo_stack.empty()) return;
        undo_stack.push_back({scene_objects, selected_index});
        scene_objects  = redo_stack.back().objects;
        selected_index = redo_stack.back().selected;
        redo_stack.pop_back();
        scene_dirty = true;
    };

    // ── Scene actions ─────────────────────────────────────────────────────────
    auto do_save = [&]() {
        save_scene(scene_objects, "scene.pscene", status_msg);
        status_until = SDL_GetTicks() + 2000;
        if (status_msg == "Saved.") {
            scene_dirty  = false;
            current_file = "scene.pscene";
        }
    };

    auto do_load = [&]() {
        load_scene(scene_objects, selected_index, "scene.pscene", status_msg);
        status_until = SDL_GetTicks() + 2000;
        if (status_msg == "Loaded.") {
            scene_dirty  = false;
            current_file = "scene.pscene";
            undo_stack.clear();
            redo_stack.clear();
        }
    };

    auto do_new = [&]() {
        push_undo();
        scene_objects.clear();
        selected_index = -1;
        scene_dirty    = false;
        current_file   = "";
    };

    auto do_add = [&]() {
        push_undo();
        SceneObject obj; obj.name = "Object";
        scene_objects.push_back(std::move(obj));
        selected_index = (int)scene_objects.size() - 1;
        scene_dirty = true;
    };

    auto do_dup = [&]() {
        if (selected_index < 0 || selected_index >= (int)scene_objects.size()) return;
        push_undo();
        SceneObject copy = scene_objects[selected_index];
        copy.name       += " (copy)";
        copy.position.x += 0.5f;
        copy.position.z += 0.5f;
        scene_objects.push_back(std::move(copy));
        selected_index = (int)scene_objects.size() - 1;
        scene_dirty = true;
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
                    default: break;
                }
            }
        }

        if (key_t)       gizmo_op = 0;
        if (key_r)       gizmo_op = 1;
        if (key_s_gizmo) gizmo_op = 2;
        if (key_undo)    do_undo();
        if (key_redo)    do_redo();
        if (key_dup)     do_dup();
        if (key_save)    do_save();
        if (key_load)    do_load();

        // ── PSX render ───────────────────────────────────────────────────────
        float     aspect = (float)PSXFramebuffer::W / (float)PSXFramebuffer::H;
        glm::mat4 vp     = cam.proj(aspect) * cam.view();

        psx_fb.bind();
        glClearColor(0.05f,0.03f,0.12f,1.f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        glUseProgram(mesh_prog);
        glUniform1f(u_snap, 60.f);
        for (const SceneObject& obj : scene_objects) {
            if (!obj.visible) continue;
            glm::mat4 model = glm::translate(glm::mat4(1.f), obj.position);
            model = glm::rotate(model, glm::radians(obj.rotation.x), {1,0,0});
            model = glm::rotate(model, glm::radians(obj.rotation.y), {0,1,0});
            model = glm::rotate(model, glm::radians(obj.rotation.z), {0,0,1});
            model = glm::scale(model, obj.scale);
            glm::mat4 mvp = vp * model;
            MeshEntry& me = get_mesh(obj.mesh_path);
            GLuint     tex = get_tex(obj.texture_path);
            glUniformMatrix4fv(u_mvp,1,GL_FALSE,glm::value_ptr(mvp));
            glUniform1i(u_use_tex, tex ? 1 : 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform1i(u_tex, 0);
            glBindVertexArray(me.vao);
            glDrawArrays(GL_TRIANGLES, 0, me.vert_count);
        }
        glBindVertexArray(0);

        // Wireframe collision overlay for selected visible object
        if (selected_index >= 0 && selected_index < (int)scene_objects.size()) {
            const SceneObject& sel = scene_objects[selected_index];
            if (sel.visible && sel.collision != "none") {
                MeshEntry& me = get_mesh(sel.mesh_path);
                glm::mat4 model = glm::translate(glm::mat4(1.f), sel.position);
                model = glm::rotate(model, glm::radians(sel.rotation.x), {1,0,0});
                model = glm::rotate(model, glm::radians(sel.rotation.y), {0,1,0});
                model = glm::rotate(model, glm::radians(sel.rotation.z), {0,0,1});
                model = glm::scale(model, sel.scale);
                glm::mat4 wire_mvp = vp * model;
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
                if (ImGui::MenuItem("New"))             do_new();
                if (ImGui::MenuItem("Open..","Ctrl+O")) do_load();
                if (ImGui::MenuItem("Save","Ctrl+S"))   do_save();
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
                if (ImGui::MenuItem("Duplicate Object","Ctrl+D",false,selected_index>=0))
                    do_dup();
                if (ImGui::MenuItem("Delete Object",nullptr,false,selected_index>=0))
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

            ImVec2 avail = ImGui::GetContentRegionAvail();
            float fw = avail.x, fh = avail.y;
            if (fh>0.f && fw/fh > 4.f/3.f) fw = fh*(4.f/3.f);
            else if (fw>0.f)                fh = fw*(3.f/4.f);
            ImVec2 cur = ImGui::GetCursorPos();
            ImGui::SetCursorPos({cur.x+(avail.x-fw)*0.5f, cur.y+(avail.y-fh)*0.5f});
            ImVec2 img_screen = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(intptr_t)psx_fb.tex, {fw,fh}, {0,1},{1,0});

#ifdef HAVE_IMGUIZMO
            if (selected_index>=0 && selected_index<(int)scene_objects.size()) {
                SceneObject& sel = scene_objects[selected_index];
                glm::mat4 view_mat = cam.view();
                glm::mat4 proj_mat = cam.proj(fw/(fh>0?fh:1.f));
                glm::mat4 model = glm::translate(glm::mat4(1.f), sel.position);
                model = glm::rotate(model,glm::radians(sel.rotation.x),{1,0,0});
                model = glm::rotate(model,glm::radians(sel.rotation.y),{0,1,0});
                model = glm::rotate(model,glm::radians(sel.rotation.z),{0,0,1});
                model = glm::scale(model, sel.scale);

                ImGuizmo::SetOrthographic(cam.ortho);
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(img_screen.x, img_screen.y, fw, fh);
                static const ImGuizmo::OPERATION ops[] = {
                    ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE
                };
                ImGuizmo::Manipulate(
                    glm::value_ptr(view_mat), glm::value_ptr(proj_mat),
                    ops[gizmo_op], ImGuizmo::LOCAL, glm::value_ptr(model));

                bool gizmo_using_now = ImGuizmo::IsUsing();
                if (gizmo_using_now && !gizmo_was_using) push_undo();
                if (gizmo_using_now) {
                    float t[3],r[3],s[3];
                    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(model),t,r,s);
                    sel.position = {t[0],t[1],t[2]};
                    sel.rotation = {r[0],r[1],r[2]};
                    sel.scale    = {s[0],s[1],s[2]};
                    scene_dirty  = true;
                }
                gizmo_was_using = gizmo_using_now;
            }
#endif
        }
        ImGui::End();

        // ── Outliner ──────────────────────────────────────────────────────────
        ImGui::Begin("Outliner");
        if (ImGui::Button("+")) ImGui::OpenPopup("add_object_popup");
        if (ImGui::BeginPopup("add_object_popup")) {
            if (ImGui::MenuItem("From File")) do_add();
            ImGui::Separator();
            struct { const char* label; const char* key; } prims[] = {
                {"Cube",     "__primitive_cube__"},
                {"Plane",    "__primitive_plane__"},
                {"Sphere",   "__primitive_sphere__"},
                {"Cylinder", "__primitive_cylinder__"},
                {"Cone",     "__primitive_cone__"},
            };
            for (auto& p : prims) {
                if (ImGui::MenuItem(p.label)) {
                    push_undo();
                    SceneObject obj;
                    obj.name         = p.label;
                    obj.mesh_path    = p.key;
                    obj.texture_path = "";
                    obj.collision    = "box";
                    scene_objects.push_back(std::move(obj));
                    selected_index = (int)scene_objects.size() - 1;
                    scene_dirty    = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();
        for (int i = 0; i < (int)scene_objects.size(); ++i) {
            SceneObject& obj = scene_objects[i];
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.1f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.2f));
            if (ImGui::SmallButton(obj.visible ? "[v]" : "[ ]")) {
                push_undo();
                obj.visible = !obj.visible;
                scene_dirty = true;
            }
            ImGui::PopStyleColor(3);
            ImGui::PopID();
            ImGui::SameLine();
            bool sel = (i == selected_index);
            if (ImGui::Selectable(obj.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
                selected_index = sel ? -1 : i;
        }
        if (!status_msg.empty() && SDL_GetTicks() < status_until)
            ImGui::TextDisabled("%s", status_msg.c_str());
        ImGui::End();

        // Deferred delete
        if (key_del && selected_index>=0 && selected_index<(int)scene_objects.size()) {
            push_undo();
            scene_objects.erase(scene_objects.begin() + selected_index);
            selected_index = selected_index>0 ? selected_index-1 : -1;
            scene_dirty = true;
        }

        // ── Inspector ─────────────────────────────────────────────────────────
        ImGui::Begin("Inspector");
        if (selected_index>=0 && selected_index<(int)scene_objects.size()) {
            SceneObject& obj = scene_objects[selected_index];

            // Name
            char name_buf[256];
            std::snprintf(name_buf, sizeof(name_buf), "%s", obj.name.c_str());
            if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
                obj.name = name_buf; scene_dirty = true;
            }
            if (ImGui::IsItemActivated()) push_undo();

            // Assets
            ImGui::Separator(); ImGui::Text("Assets");

            auto path_field = [&](const char* label, std::string& path,
                                  const char* dir,
                                  std::initializer_list<const char*> exts,
                                  const char* fid) {
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", label); ImGui::SameLine();
                char buf[512]; std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 35.f);
                char in_id[32], btn_id[32];
                std::snprintf(in_id,  sizeof(in_id),  "##%s_in",  fid);
                std::snprintf(btn_id, sizeof(btn_id),  "...##%s",  fid);
                if (ImGui::InputText(in_id, buf, sizeof(buf))) {
                    path = buf; scene_dirty = true;
                }
                if (ImGui::IsItemActivated()) push_undo();
                ImGui::SameLine();
                if (ImGui::Button(btn_id)) picker.open(&path, dir, exts);
            };

            path_field("Mesh",    obj.mesh_path,    "assets/meshes/",   {".obj"},            "mesh");
            {
                auto wit = validation_warnings.find(obj.mesh_path);
                if (wit != validation_warnings.end())
                    for (const auto& w : wit->second)
                        ImGui::TextColored({1.f, 0.85f, 0.f, 1.f}, "! %s", w.c_str());
            }
            path_field("Texture", obj.texture_path, "assets/textures/", {".tga",".png",".bmp"}, "tex");

            // Transform
            ImGui::Separator(); ImGui::Text("Transform");
            ImGui::DragFloat3("Position", &obj.position.x, 0.1f);
            if (ImGui::IsItemActivated()) push_undo();
            if (ImGui::IsItemEdited())    scene_dirty = true;

            ImGui::DragFloat3("Rotation", &obj.rotation.x, 1.0f);
            if (ImGui::IsItemActivated()) push_undo();
            if (ImGui::IsItemEdited())    scene_dirty = true;

            ImGui::DragFloat3("Scale", &obj.scale.x, 0.01f, 0.001f, FLT_MAX, "%.3f");
            if (ImGui::IsItemActivated()) push_undo();
            if (ImGui::IsItemEdited())    scene_dirty = true;

            // Physics
            ImGui::Separator(); ImGui::Text("Physics");
            const char* coll_items[] = {"none","box","mesh","convex"};
            int coll_idx = 0;
            for (int ci = 0; ci < 4; ++ci)
                if (obj.collision == coll_items[ci]) { coll_idx = ci; break; }
            if (ImGui::Combo("Collision", &coll_idx, coll_items, 4)) {
                push_undo();
                obj.collision = coll_items[coll_idx];
                scene_dirty   = true;
            }

#ifdef HAVE_IMGUIZMO
            ImGui::Separator();
            ImGui::TextDisabled("T=translate  R=rotate  S=scale");
#endif
        } else {
            ImGui::TextDisabled("No object selected");
        }

        // File picker modal
        if (ImGui::BeginPopupModal("File Picker", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Select file:"); ImGui::Separator();
            if (picker.files.empty()) {
                ImGui::TextDisabled("No files found");
            } else {
                for (const auto& fp : picker.files) {
                    if (ImGui::Selectable(fp.c_str())) {
                        push_undo();
                        if (picker.dest) *picker.dest = fp;
                        picker.dest  = nullptr;
                        scene_dirty  = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::Button("Cancel", {120,0})) ImGui::CloseCurrentPopup();
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
            std::string title = "psx-editor \xe2\x80\x94 "
                + (current_file.empty() ? std::string("untitled") : current_file)
                + (scene_dirty ? " *" : "");
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
