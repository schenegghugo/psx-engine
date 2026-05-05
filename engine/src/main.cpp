// psx-engine — runtime entry point
// Milestone 4 step 2: renders all scene_objects with mesh+texture caches
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
#include <filesystem>

#include <SDL2/SDL.h>

#if __has_include(<glad/gl.h>)
#  include <glad/gl.h>
#  define GLAD2 1
#else
#  include <glad/glad.h>
#  define GLAD2 0
#endif

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <nlohmann/json.hpp>

// ── Helpers ───────────────────────────────────────────────────────────────────
static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open file: %s\n", path); return ""; }
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
        fprintf(stderr, "Shader error:\n%s\n", log);
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
        fprintf(stderr, "Link error:\n%s\n", log);
    }
    glDeleteShader(vert); glDeleteShader(frag);
    return p;
}

// ── TGA writer ────────────────────────────────────────────────────────────────
static void write_checkerboard_tga(const char* path, int size, int cell) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot write TGA: %s\n", path); return; }
    uint8_t hdr[18] = {};
    hdr[2]  = 2;
    hdr[12] = (uint8_t)(size & 0xFF);  hdr[13] = (uint8_t)((size >> 8) & 0xFF);
    hdr[14] = (uint8_t)(size & 0xFF);  hdr[15] = (uint8_t)((size >> 8) & 0xFF);
    hdr[16] = 24;  hdr[17] = 0x20;
    fwrite(hdr, 1, 18, f);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            uint8_t c = (((x / cell) ^ (y / cell)) & 1) ? 220 : 32;
            uint8_t px[3] = {c, c, c};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "TGA: wrote %dx%d checkerboard -> %s\n", size, size, path);
}

// ── TGA loader ────────────────────────────────────────────────────────────────
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
        fprintf(stderr, "TGA: unsupported type=%d bpp=%d in %s\n", imgtype, bpp, path);
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
        for (int row = 0; row < h / 2; ++row) {
            int mirror = h - 1 - row;
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    std::swap(img.rgb[(row*w+x)*3+c], img.rgb[(mirror*w+x)*3+c]);
        }
    }
    fprintf(stderr, "TGA: loaded %dx%d %s\n", w, h, path);
    return img;
}

// ── OBJ loader: stride 8 — pos(3) uv(2) colour(3) ────────────────────────────
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
    std::vector<std::array<FV, 3>> tris;

    static const glm::vec3 PALETTE[] = {
        {1.0f,0.35f,0.35f}, {0.35f,1.0f,0.35f}, {0.35f,0.35f,1.0f},
        {1.0f,1.0f,0.35f},  {1.0f,0.35f,1.0f},  {0.35f,1.0f,1.0f},
    };

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line); std::string tok; ss >> tok;
        if (tok == "v") {
            float x, y, z; ss >> x >> y >> z; positions.push_back({x, y, z});
        } else if (tok == "vt") {
            float u, v; ss >> u >> v; uvs.push_back({u, v});
        } else if (tok == "f") {
            std::vector<FV> poly; std::string t;
            while (ss >> t) {
                size_t s1 = t.find('/');
                int vi = std::stoi(t.substr(0, s1));
                if (vi < 0) vi = (int)positions.size() + vi + 1;
                vi -= 1;
                int vti = -1;
                if (s1 != std::string::npos) {
                    size_t s2 = t.find('/', s1 + 1);
                    std::string vts = t.substr(s1+1,
                        s2 == std::string::npos ? std::string::npos : s2-s1-1);
                    if (!vts.empty()) {
                        vti = std::stoi(vts);
                        if (vti < 0) vti = (int)uvs.size() + vti + 1;
                        vti -= 1;
                    }
                }
                poly.push_back({vi, vti});
            }
            for (int i = 1; i + 1 < (int)poly.size(); ++i)
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
                {p.x, p.y, p.z, uv.x, uv.y, col.r, col.g, col.b});
        }
    }
    fprintf(stderr, "OBJ: %d tris (%s)\n", (int)tris.size(), path);
    return result;
}

// ── Built-in primitives ───────────────────────────────────────────────────────
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
            float x = cosf(lat)*cosf(lon), y = sinf(lat), z = cosf(lat)*sinf(lon);
            float u = lon/(2.f*PI), v = lat/PI+0.5f;
            r.verts.insert(r.verts.end(), {x, y, z, u, v, c[0], c[1], c[2]});
        };
        for (int i = 0; i < stacks; ++i) {
            float lat0 = PI*(-0.5f+(float)i/stacks);
            float lat1 = PI*(-0.5f+(float)(i+1)/stacks);
            for (int j = 0; j < slices; ++j) {
                float lon0 = 2.f*PI*(float)j/slices;
                float lon1 = 2.f*PI*(float)(j+1)/slices;
                push(lat0,lon0); push(lat1,lon0); push(lat1,lon1);
                push(lat0,lon0); push(lat1,lon1); push(lat0,lon1);
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
            float a0 = 2.f*PI*(float)j/slices,  a1 = 2.f*PI*(float)(j+1)/slices;
            float x0 = cosf(a0), z0 = sinf(a0), x1 = cosf(a1), z1 = sinf(a1);
            float u0 = (float)j/slices,          u1 = (float)(j+1)/slices;
            push(x0,-1,z0,u0,0); push(x1,-1,z1,u1,0); push(x1,1,z1,u1,1);
            push(x0,-1,z0,u0,0); push(x1,1,z1,u1,1);  push(x0,1,z0,u0,1);
            push(0,1,0,0.5f,0.5f); push(x0,1,z0,u0,0); push(x1,1,z1,u1,0);
            push(0,-1,0,0.5f,0.5f); push(x1,-1,z1,u1,0); push(x0,-1,z0,u0,0);
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
            float a0 = 2.f*PI*(float)j/slices,  a1 = 2.f*PI*(float)(j+1)/slices;
            float x0 = cosf(a0), z0 = sinf(a0), x1 = cosf(a1), z1 = sinf(a1);
            float u0 = (float)j/slices,          u1 = (float)(j+1)/slices;
            push(0,1,0,0.5f,1.f); push(x0,-1,z0,u0,0); push(x1,-1,z1,u1,0);
            push(0,-1,0,0.5f,0.5f); push(x1,-1,z1,u1,0); push(x0,-1,z0,u0,0);
        }
        return r;
    }

    return make_unit_cube();
}

// ── GPU mesh ──────────────────────────────────────────────────────────────────
struct MeshEntry {
    GLuint vao = 0, vbo = 0;
    int    vert_count = 0;
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

// ── Free-look camera ──────────────────────────────────────────────────────────
struct Camera {
    glm::vec3 pos   = {0.f, 0.f, 5.f};
    float     yaw   = -90.f;
    float     pitch = 0.f;
    float     speed = 5.f;
    float     sens  = 0.1f;

    glm::vec3 front() const {
        return glm::normalize(glm::vec3{
            cosf(glm::radians(yaw)) * cosf(glm::radians(pitch)),
            sinf(glm::radians(pitch)),
            sinf(glm::radians(yaw)) * cosf(glm::radians(pitch))
        });
    }

    glm::mat4 view() const {
        return glm::lookAt(pos, pos + front(), glm::vec3{0.f, 1.f, 0.f});
    }

    void move(const Uint8* keys, float dt) {
        glm::vec3 fr = front();
        glm::vec3 r  = glm::normalize(glm::cross(fr, glm::vec3{0.f, 1.f, 0.f}));
        if (keys[SDL_SCANCODE_W]) pos += fr * speed * dt;
        if (keys[SDL_SCANCODE_S]) pos -= fr * speed * dt;
        if (keys[SDL_SCANCODE_A]) pos -= r  * speed * dt;
        if (keys[SDL_SCANCODE_D]) pos += r  * speed * dt;
    }

    void look(float dx, float dy) {
        yaw   += dx * sens;
        pitch -= dy * sens;
        pitch  = glm::clamp(pitch, -89.f, 89.f);
    }
};

// ── Scene object (mirrors editor's SceneObject) ───────────────────────────────
struct SceneObject {
    std::string name;
    std::string mesh_path    = "assets/meshes/test.obj";
    std::string texture_path = "assets/textures/test.tga";
    bool        visible      = true;
    glm::vec3   position     = {0.f, 0.f, 0.f};
    glm::vec3   rotation     = {0.f, 0.f, 0.f};  // degrees
    glm::vec3   scale        = {1.f, 1.f, 1.f};
    std::string collision    = "box";
};

static std::vector<SceneObject> load_pscene(const char* path) {
    using json = nlohmann::json;
    std::ifstream f(path);
    if (!f) return {};
    json root;
    try { root = json::parse(f); } catch (...) { return {}; }

    std::vector<SceneObject> result;
    for (const auto& o : root.value("objects", json::array())) {
        SceneObject obj;
        obj.name         = o.value("name",         "Object");
        obj.mesh_path    = o.value("mesh_path",    "assets/meshes/test.obj");
        obj.texture_path = o.value("texture_path", "assets/textures/test.tga");
        obj.visible      = o.value("visible",      true);
        obj.collision    = o.value("collision",    "box");
        auto get3 = [&](const char* key, glm::vec3 def) -> glm::vec3 {
            auto v = o.value(key, std::vector<float>{def.x, def.y, def.z});
            return v.size() == 3 ? glm::vec3{v[0], v[1], v[2]} : def;
        };
        obj.position = get3("position", {0,0,0});
        obj.rotation = get3("rotation", {0,0,0});
        obj.scale    = get3("scale",    {1,1,1});
        result.push_back(std::move(obj));
    }
    return result;
}

// ── PSX framebuffer (320×240 → nearest-neighbour upscale) ────────────────────
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

static const float QUAD_VERTS[] = {
    -1,-1,0, 0,0,  1,-1,0, 1,0,  1,1,0, 1,1,
    -1,-1,0, 0,0,  1, 1,0, 1,1, -1,1,0, 0,1,
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
            std::string head(4096, '\0'); probe.read(head.data(), 4096);
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
                fprintf(stderr, "OBJ: wrote default cube -> %s\n", mp);
            }
        }
    }

    // ── Load scene ────────────────────────────────────────────────────────────
    std::vector<SceneObject> scene_objects = load_pscene("scene.pscene");
    if (!scene_objects.empty()) {
        fprintf(stderr, "scene: loaded %zu objects from scene.pscene\n",
                scene_objects.size());
    } else {
        fprintf(stderr, "scene: scene.pscene not found or malformed, using default\n");
        scene_objects.push_back(SceneObject{});
    }

    // ── SDL / GL ──────────────────────────────────────────────────────────────
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* win = SDL_CreateWindow(
        "psx-engine",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "GLAD: failed\n"); return 1;
    }
    fprintf(stderr, "OpenGL %s  |  %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    PSXFramebuffer psx_fb; psx_fb.init();

    // ── Mesh + texture caches ─────────────────────────────────────────────────
    std::map<std::string, MeshEntry> mesh_cache;
    std::map<std::string, GLuint>    tex_cache;

    // Seed built-in primitives
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
        if (lm.verts.empty()) lm = make_unit_cube();
        mesh_cache[path] = upload_mesh(lm);
        return mesh_cache[path];
    };

    auto get_tex = [&](const std::string& path) -> GLuint {
        if (path.empty()) return 0;
        auto it = tex_cache.find(path);
        if (it != tex_cache.end()) return it->second;
        TGAImage img = load_tga(path.c_str());
        GLuint tex = 0;
        if (!img.rgb.empty()) {
            glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, img.w, img.h, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, img.rgb.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        tex_cache[path] = tex;
        return tex;
    };

    // ── Blit quad ─────────────────────────────────────────────────────────────
    GLuint quad_vao, quad_vbo;
    glGenVertexArrays(1, &quad_vao); glGenBuffers(1, &quad_vbo);
    glBindVertexArray(quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTS), QUAD_VERTS, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glBindVertexArray(0);

    // ── Shaders ───────────────────────────────────────────────────────────────
    auto load_prog = [&](const char* v, const char* f) {
        std::string vs = read_file(v), fs = read_file(f);
        return link_program(compile_shader(GL_VERTEX_SHADER,   vs.c_str()),
                            compile_shader(GL_FRAGMENT_SHADER, fs.c_str()));
    };
    GLuint mesh_prog = load_prog("shaders/psx.vert",  "shaders/psx.frag");
    GLuint blit_prog = load_prog("shaders/blit.vert", "shaders/blit.frag");

    GLint u_mvp      = glGetUniformLocation(mesh_prog, "u_mvp");
    GLint u_snap     = glGetUniformLocation(mesh_prog, "u_snap_resolution");
    GLint u_use_tex  = glGetUniformLocation(mesh_prog, "u_use_texture");
    GLint u_tex      = glGetUniformLocation(mesh_prog, "u_texture");
    GLint u_blit_tex = glGetUniformLocation(blit_prog, "u_texture");

    // ── Runtime state ─────────────────────────────────────────────────────────
    Camera cam;
    bool  snap_on        = true;
    float snap_res       = 60.f;
    bool  show_ui        = true;
    bool  mouse_captured = false;

    Uint32 last    = SDL_GetTicks();
    bool   running = true;

    while (running) {
        Uint32 now = SDL_GetTicks();
        float  dt  = (now - last) / 1000.f;
        last = now;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_F1) show_ui = !show_ui;
                if (ev.key.keysym.sym == SDLK_ESCAPE && mouse_captured) {
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                    mouse_captured = false;
                }
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN && !mouse_captured) {
                SDL_SetRelativeMouseMode(SDL_TRUE);
                mouse_captured = true;
            }
            if (ev.type == SDL_MOUSEMOTION && mouse_captured)
                cam.look((float)ev.motion.xrel, (float)ev.motion.yrel);
        }

        if (mouse_captured && !io.WantCaptureKeyboard)
            cam.move(SDL_GetKeyboardState(nullptr), dt);

        float     aspect = (float)PSXFramebuffer::W / (float)PSXFramebuffer::H;
        glm::mat4 proj   = glm::perspective(glm::radians(60.f), aspect, 0.1f, 100.f);
        glm::mat4 view   = cam.view();

        // ── Pass 1 — scene objects → PSX 320×240 FBO ─────────────────────────
        psx_fb.bind();
        glClearColor(0.05f, 0.03f, 0.12f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        glUseProgram(mesh_prog);
        glUniform1f(u_snap, snap_on ? snap_res : 0.f);
        glUniform1i(u_tex, 0);
        glActiveTexture(GL_TEXTURE0);

        int total_tris = 0;
        for (const SceneObject& obj : scene_objects) {
            if (!obj.visible) continue;
            glm::mat4 model = glm::translate(glm::mat4(1.f), obj.position);
            model = glm::rotate(model, glm::radians(obj.rotation.x), {1,0,0});
            model = glm::rotate(model, glm::radians(obj.rotation.y), {0,1,0});
            model = glm::rotate(model, glm::radians(obj.rotation.z), {0,0,1});
            model = glm::scale(model, obj.scale);
            glUniformMatrix4fv(u_mvp, 1, GL_FALSE, glm::value_ptr(proj * view * model));
            MeshEntry& me = get_mesh(obj.mesh_path);
            GLuint tex = get_tex(obj.texture_path);
            glUniform1i(u_use_tex, tex ? 1 : 0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glBindVertexArray(me.vao);
            glDrawArrays(GL_TRIANGLES, 0, me.vert_count);
            total_tris += me.vert_count / 3;
        }
        glBindVertexArray(0);
        psx_fb.unbind();

        // ── Pass 2 — blit to window ───────────────────────────────────────────
        int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
        glViewport(0, 0, ww, wh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);

        glUseProgram(blit_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, psx_fb.tex);
        glUniform1i(u_blit_tex, 0);
        glBindVertexArray(quad_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // ── ImGui overlay ─────────────────────────────────────────────────────
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (show_ui) {
            ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
            ImGui::SetNextWindowSize({340, 180}, ImGuiCond_Once);
            ImGui::Begin("PSX debug  [F1 toggle]");
            ImGui::Text("%.1f fps", io.Framerate);
            ImGui::Text("pos (%.1f, %.1f, %.1f)  yaw %.0f  pitch %.0f",
                        cam.pos.x, cam.pos.y, cam.pos.z, cam.yaw, cam.pitch);
            ImGui::Text("%s", mouse_captured
                ? "Mouse captured — Esc to release"
                : "Click window to capture mouse");
            ImGui::Separator();
            ImGui::Checkbox("Vertex snap", &snap_on);
            if (snap_on) ImGui::SliderFloat("Snap res", &snap_res, 8.f, 256.f);
            ImGui::Text("FBO 320x240  objects %zu  tris %d",
                        scene_objects.size(), total_tris);
            ImGui::End();
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    psx_fb.destroy();
    for (auto& [k, me] : mesh_cache) {
        if (me.vao) glDeleteVertexArrays(1, &me.vao);
        if (me.vbo) glDeleteBuffers(1, &me.vbo);
    }
    for (auto& [k, tex] : tex_cache) if (tex) glDeleteTextures(1, &tex);
    glDeleteVertexArrays(1, &quad_vao); glDeleteBuffers(1, &quad_vbo);
    glDeleteProgram(mesh_prog); glDeleteProgram(blit_prog);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
