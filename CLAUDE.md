# CLAUDE.md
This file provides guidance to Claude Code when working with this repository.

# psx-engine
C++ PSX-aesthetic 3D engine + level editor. Two executables:
- `engine/` → `psx_engine` — runtime, gameplay executable
- `editor/` → `psx_editor` — level editor UI (non-coder facing)

---

## Bootstrap (first time on a new machine)
```bash
cd ~/Games
bash psxSetup.sh psx-engine
```
Installs pacman packages (cmake, ninja, mesa, autoconf, automake, libtool,
mingw-w64-gcc, python-glad via AUR), clones vcpkg, generates GLAD, vendors
ImGui (docking branch), writes all source/shader/CMake files, first build.
Aborts if target directory already exists.

---

## Build
```bash
bash build-linux.sh          # Arch/Wayland → build/linux/
bash build-windows.sh        # Windows cross-compile via MinGW → build/windows/
```
```bash
cmake --build build/linux --target psx_engine --parallel   # single target
cmake --build build/linux --target psx_editor --parallel
rm -rf build/linux && bash build-linux.sh                  # full wipe + rebuild
```

---

## Run
Binaries must run from their own directory — shaders/ and assets/ are
copied there at build time and loaded relative to cwd:
```bash
cd build/linux/engine && ./psx_engine    # F1 = ImGui overlay
cd build/linux/editor && ./psx_editor    # full level editor
```

---

## Stack
| Concern | Library | Notes |
|---|---|---|
| Window / input | SDL2 (vcpkg) | Wayland-native via EGL |
| GL loader | GLAD glad1/0.1.x (vendored) | `vendor/glad/` — no GLX |
| GL | OpenGL 3.3 core | Mesa 26.x / 4.6 on T480 |
| UI | Dear ImGui docking (vendored) | `vendor/imgui/` |
| Gizmo | ImGuizmo (vendored) | `vendor/imguizmo/` |
| Math | GLM (vcpkg) | |
| ECS | EnTT (vcpkg) | linked, not yet used |
| JSON | nlohmann_json (vcpkg) | .pscene format |
| Build | CMake 3.22+ + Ninja | `LANGUAGES C CXX` — C needed for glad.c |
| Optimisation | `-O3 -march=native -funroll-loops` | Release only |

GLAD loads via `gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)` —
works on EGL/Wayland, GLX/X11, and WGL/Windows. Never use GLEW.

---

## Repository layout
```
shared/
  scene_format.hpp        psx::Node, psx::Scene (tree-based, milestone 4+)

engine/
  src/main.cpp            SDL2 + GLAD init, two-pass render loop, ImGui overlay
  shaders/
    psx.vert              MVP, vertex snap, affine UV warp
    psx.frag              texture * vertex colour
    blit.vert / blit.frag fullscreen quad upscale

editor/
  src/main.cpp            full editor implementation
  shaders/                copies of engine shaders

vendor/
  glad/                   OpenGL loader
  imgui/                  Dear ImGui docking branch
  imguizmo/               3D gizmo

assets/
  meshes/test.obj         unit cube with UVs
  textures/test.tga       64x64 checkerboard

cmake/
  toolchain-mingw.cmake   Windows cross-compile
```

---

## Render pipeline (engine/src/main.cpp)
**Pass 1 — PSX FBO**
- Bind `PSXFramebuffer` (320x240, `GL_NEAREST`, `GL_DEPTH24_STENCIL8`)
- `glEnable(GL_DEPTH_TEST)`, draw scene meshes
- Uniforms: `u_mvp` (mat4), `u_snap_resolution` (float), `u_use_texture` (int), `u_texture` (sampler2D)

**Pass 2 — blit**
- Unbind FBO, fullscreen quad, `GL_NEAREST` upscale to window

**ImGui** — `NewFrame` → panels → `Render` → `RenderDrawData` → `SDL_GL_SwapWindow`

---

## Vertex layout (stride = 8 x float)
| attrib | location | components | offset |
|---|---|---|---|
| position | 0 | 3 floats | 0 |
| uv | 1 | 2 floats | 12 |
| colour | 2 | 3 floats | 20 |

`mesh_vert_count = mesh_data.size() / 8`

---

## PSX aesthetic rules
- **Vertex snap**: `floor(ndc * half_res + 0.5) / half_res` in `psx.vert`
- **320x240 FBO** + `GL_NEAREST` upscale
- **Affine texture warp**: `v_uv = a_uv * clip.w` — perspective-incorrect, the swim effect
- **Depth buffer**: GL_DEPTH24_STENCIL8 active
- **Texture blend**: `texture(u_tex, v_uv / v_uv_w) * vec4(v_colour, 1.0)`

---

## Scene format (.pscene v2 — tree-based)

The current flat `objects[]` schema is being upgraded to a node tree in
milestone 4. The loader handles both v1 (objects array) and v2 (nodes
array) — v1 files auto-migrate on load.

### Schema v2

```json
{
  "format_version": 2,
  "name": "level1",
  "nodes": [
    {
      "id": 0,
      "name": "Level",
      "kind": "node",
      "parent": -1,
      "children": [1, 2],
      "transform": {
        "position": [0, 0, 0],
        "rotation": [0, 0, 0],
        "scale": [1, 1, 1]
      },
      "components": {}
    },
    {
      "id": 1,
      "name": "Floor",
      "kind": "mesh",
      "parent": 0,
      "children": [],
      "transform": {...},
      "components": {
        "mesh": { "path": "assets/meshes/floor.obj", "texture": "assets/textures/test.tga" },
        "collision": { "type": "mesh" }
      }
    },
    {
      "id": 2,
      "name": "Player",
      "kind": "player",
      "parent": 0,
      "children": [3, 4],
      "transform": {...},
      "components": {
        "spawn": { "is_default": true }
      }
    },
    {
      "id": 3,
      "name": "Camera",
      "kind": "camera",
      "parent": 2,
      "children": [],
      "transform": {...},
      "components": {
        "camera": { "fov": 60, "near": 0.1, "far": 100 }
      }
    },
    {
      "id": 4,
      "name": "Capsule",
      "kind": "mesh",
      "parent": 2,
      "children": [],
      "transform": {...},
      "components": {
        "mesh": { "path": "__primitive_cylinder__", "texture": "" },
        "collision": { "type": "convex" }
      }
    }
  ]
}
```

### Node kinds

| kind | typical components | purpose |
|---|---|---|
| `node` | (any) | empty container, group / pivot |
| `mesh` | `mesh`, optional `collision` | renders geometry |
| `camera` | `camera` | viewpoint, can be active |
| `light` | `light` | contributes to lighting pass (m7+) |
| `player` | `spawn`, optional `controller` | player rig root |

### Components (data-only attachments)

| name | fields |
|---|---|
| `mesh` | `path` (OBJ or `__primitive_*__`), `texture` (TGA path or empty) |
| `collision` | `type`: `none` / `box` / `mesh` / `convex` |
| `camera` | `fov` (degrees), `near`, `far`, optional `is_active` (bool) |
| `light` | `type` (directional/point), `color` [r,g,b], `intensity` |
| `spawn` | `is_default` (bool) — default spawn point if multiple |
| `controller` | `type` (fps / orbit / scripted), `speed`, `mouse_sensitivity` |

### Transforms

Every node stores a **local** transform (position, rotation Euler XYZ
degrees, scale) relative to its parent. World transform = chain of parent
transforms walked from root. Moving a Player node moves its Camera and
Capsule children automatically.

### Migration from v1

Loader detects `objects[]` (v1) vs `nodes[]` (v2). v1 files become a
single root `node` with one `mesh` child per old SceneObject, preserving
all transforms and collision settings.

---

## Milestone status

### Milestone 0 — triangle (DONE)
SDL2 + GLAD + ImGui + PSX FBO pipeline.

### Milestone 1 — OBJ + camera (DONE)
Manual OBJ loader, free-look camera (WASD + mouse), MVP uniform.

### Milestone 2 — textures (DONE)
Manual TGA loader, programmatic checkerboard, affine UV warp.

### Milestone 3 — Level Editor MVP (DONE)
Full editor: dockspace (Viewport/Outliner/Inspector), orbit camera with
Blender keybinds, ImGuizmo translate/rotate/scale, primitives (cube/plane
/sphere/cylinder/cone), file pickers, OBJ validation (PSX triangle/UV
budget), undo/redo, save/load .pscene v1, menubar, duplicate, dirty title.

---

### Milestone 4 — Scene tree (Node + components)  [CURRENT]

**Why:** Currently SceneObject is flat — Cube/Light/Spawn Point all render
as a cube because there is no concept of node *kind*. Real engines split
**what an object is** from **what mesh it draws**, and use a parent/child
**tree** so parenting "just works" (move Player → Camera and Capsule follow).

**Architectural decisions:**
- **Composition over inheritance.** A `Node` is `{ id, name, kind, transform, parent, children, components{} }`. Specialisation is data, not subclasses. Same approach as Godot.
- **Local transforms only stored.** World transform = walk up the parent chain. Recomputed each frame for animated nodes; cached for static.
- **Components are JSON objects keyed by name.** Adding a new component type means: define its schema, add an Inspector panel for it, optionally wire it into the renderer/runtime.
- **The editor already produces a flat list — we keep .pscene v1 readable indefinitely.** v1 files load as a flat tree (single root node + N mesh children).

**Implementation order — one step per prompt, build between each:**

#### Step 1 of 7 — `shared/scene_format.hpp` v2
- Define `psx::Node`, `psx::Component` (variant of mesh/collision/camera/light/spawn/controller), `psx::Scene` with `nodes` vector
- JSON serialise/deserialise via nlohmann_json
- v1 → v2 migration helper: `Scene::from_v1(json)`
- Loader detects schema by checking for `format_version` or `nodes` key
- Both targets include from `shared/`

#### Step 2 of 7 — Editor outliner becomes a tree
- Replace flat list with `ImGui::TreeNodeEx` per node
- Indentation by depth, expand/collapse triangles
- Selection state moved to `selected_node_id` (instead of index)
- Visibility toggle `[v]/[ ]` retained per node
- "+" button still works — adds new node as child of selected (or root if nothing selected)

#### Step 3 of 7 — Add by kind (Empty / Mesh / Camera / Light / Player)
- "+" dropdown becomes a kind picker:
  - `Empty Node` — kind=node, no components
  - `Mesh > From File / Cube / Plane / Sphere / Cylinder / Cone` (current primitives, packaged as mesh nodes)
  - `Camera`
  - `Light > Directional / Point`
  - `Player` (kind=player, child Camera auto-added, child Capsule auto-added)
- New nodes get unique auto-incrementing IDs

#### Step 4 of 7 — Per-kind Inspector
- Inspector panel switches layout based on `selected.kind`:
  - **All kinds**: name, transform (position/rotation/scale)
  - **Mesh kind**: + mesh component (path picker, texture picker, collision dropdown, validation warnings)
  - **Camera kind**: + camera component (FOV, near/far, "Set as active" button)
  - **Light kind**: + light component (type, colour picker, intensity)
  - **Player kind**: + spawn component (default toggle), + controller component (type dropdown, speed, sensitivity)
- Each component section is collapsible (`ImGui::CollapsingHeader`)

#### Step 5 of 7 — Re-parenting (drag in outliner)
- Drag-source on each tree row, drop-target on each tree row
- On drop: change `parent` field, update `children` lists on both old and new parents
- Prevent cycles (cannot parent a node to its own descendant)
- Drag onto empty area → re-parent to scene root
- Recompute world transforms automatically (already done each frame, no extra work)

#### Step 6 of 7 — Engine reads the tree
- Replace flat `scene_objects` with `Scene` from `shared/scene_format.hpp`
- Walk tree, compute world transforms
- For each `mesh` node: render with mesh + texture from its component
- Active camera selection:
  - Find `camera` node with `is_active = true`, or
  - Find `camera` child of any `player` node, or
  - Fall back to free-look camera (current engine camera)
- LightNodes contribute to a uniform array (basic; full lighting pass is m7)

#### Step 7 of 7 — Camera preview in editor (nice-to-have)
- Editor inspector for camera nodes shows a "Preview" mini-viewport
- Renders the scene from that camera's perspective at 160x120
- Refreshes only when camera transform or component changes (cheap)

---

### Milestone 5 — Engine catches up + scene path argument
- Engine command-line: `./psx_engine my_level.pscene` (default `scene.pscene`)
- Editor menubar: "Play" button → launches engine with current scene file
- Move all loaders (`load_obj`, `load_tga`, `generate_primitive`) to `shared/`
- Single source of truth across both targets

### Milestone 6 — Physics / Collision
- AABB / mesh / convex collision data already in `.pscene` (m3+)
- Runtime collision detection: AABB vs AABB, ray vs triangle, sphere vs mesh
- No physics lib — write what we need
- Editor wireframes already visible (m3 step 8)

### Milestone 7 — Lighting pass
- Vertex-coloured PSX-style lighting: per-vertex normal x light direction
- Up to 4 active lights per scene (PSX hardware limit feel)
- Ambient + directional + point lights, no shadows
- Vertex shader does the lighting maths — fragment stays simple

### Milestone 8 — Asset pipeline
- TGA → PSX-quantized texture (4-bit indexed, 256-colour palette)
- OBJ → custom binary mesh format (`.pmesh`) — fast load, smaller files
- Editor "Build assets" menu item runs the pipeline

### Milestone 9 — Audio + scripting hooks
- miniaudio integration for SFX and music
- Per-node `script` component (Lua? Wren? deferred decision)
- Engine calls script callbacks on update/collision/input events

---

## Platform
- Arch Linux, Sway/Wayland — no XWayland, no GLX
- ThinkPad T480, Intel UHD 620 (KBL GT2), Mesa 26.0.6
- `$TERMINAL` must be unset when launching Claude Code (`TERMINAL= claude`)
- vcpkg at `$HOME/vcpkg` (override: `VCPKG_DIR` env var)
- Windows: `cmake/toolchain-mingw.cmake` + triplet `x64-mingw-static`

---

## Shell rules (Claude Code)
- **Never** background a command with `&`
- **Never** use Monitor
- **Never** tail log files
- Run one command at a time, read full output before proceeding
- Use the Write tool to edit files directly — do not pipe large content via bash
- **Always** `fprintf(stderr, ...)` for debug output — stdout is swallowed by SDL/Wayland

---

## Known API gotchas
- `DockSpaceOverViewport(0, ImGui::GetMainViewport())` — first arg is dockspace_id
- `EndPopup` (not `EndPopupModal`) closes a `BeginPopupModal`
- `(SDL_Keymod)ev.key.keysym.mod` — explicit cast required for SDL_Keymod
- Emoji `u8"..."` literals are `char8_t*` in C++20 — incompatible with ImGui's `const char*`. Use ASCII `"[v]"` etc.
- `LANGUAGES C CXX` in root CMakeLists — C is required for `vendor/glad/src/glad.c`
