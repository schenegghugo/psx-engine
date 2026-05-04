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
cd build/linux/editor && ./psx_editor    # milestone 3+
```

---

## Stack
| Concern | Library | Notes |
|---|---|---|
| Window / input | SDL2 (vcpkg) | Wayland-native via EGL |
| GL loader | GLAD glad1/0.1.x (vendored) | `vendor/glad/` — no GLX |
| GL | OpenGL 3.3 core | Mesa 26.x / 4.6 on T480 |
| UI | Dear ImGui docking (vendored) | `vendor/imgui/` |
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
  scene_format.hpp        psx::Vertex, psx::SceneObject, psx::Scene
                          Must stay free of platform headers (used by both targets)

engine/
  src/main.cpp            SDL2 + GLAD init, two-pass render loop, ImGui overlay
  src/renderer/           planned
  src/scene/              planned
  src/ecs/                planned (EnTT linked, unused)
  src/audio/              planned
  shaders/
    psx.vert              PSX scene pass: MVP, vertex snap, affine UV warp
    psx.frag              texture * vertex colour, u_use_texture toggle
    blit.vert             fullscreen quad, no transform
    blit.frag             GL_NEAREST sample of PSX FBO texture

editor/
  src/main.cpp            milestone 3 implementation goes here
  src/ui/                 ImGui panels
  src/viewport/           PSX renderer reused for preview
  src/pipeline/           asset import
  shaders/                copies of engine shaders (shared PSX preview)

vendor/
  glad/
    src/glad.c
    include/glad/glad.h
    include/KHR/khrplatform.h
  imgui/
    imgui*.h / imgui*.cpp
    backends/imgui_impl_sdl2.{h,cpp}
    backends/imgui_impl_opengl3.{h,cpp}

assets/
  meshes/test.obj         cube with UVs (regenerated at startup if vt missing)
  textures/test.tga       64×64 checkerboard (generated at startup if missing)

cmake/
  toolchain-mingw.cmake   MinGW-w64 x86_64 cross-compile
```

---

## Render pipeline (engine/src/main.cpp)
Two passes every frame:

**Pass 1 — PSX FBO**
- Bind `PSXFramebuffer` (320×240, `GL_NEAREST`, `GL_DEPTH24_STENCIL8`)
- `glEnable(GL_DEPTH_TEST)`
- Draw scene meshes with `psx.vert` / `psx.frag`
- Uniforms: `u_mvp` (mat4), `u_snap_resolution` (float), `u_use_texture` (int), `u_texture` (sampler2D)

**Pass 2 — blit**
- Unbind FBO, fullscreen quad → window
- `blit.vert` / `blit.frag`, `GL_NEAREST` upscale

**ImGui**
- `NewFrame` → panels → `Render` → `RenderDrawData` → `SDL_GL_SwapWindow`

---

## Vertex layout (stride = 8 × float)
| attrib | location | components | offset |
|---|---|---|---|
| position | 0 | 3 floats | 0 |
| uv | 1 | 2 floats | 12 |
| colour | 2 | 3 floats | 20 |

`mesh_vert_count = mesh_data.size() / 8`

---

## PSX aesthetic rules
- **Vertex snap** (`psx.vert`, `u_snap_resolution`):
  `floor(ndc * half_res + 0.5) / half_res` — lower = more wobble, 0 = off
- **320×240 FBO** + `GL_NEAREST` upscale — pixelated look
- **Affine texture warp**: `v_uv = a_uv * clip.w` in `psx.vert` — skips
  perspective correction, UVs interpolate linearly in screen space (swim effect)
- **Depth buffer**: active (`GL_DEPTH24_STENCIL8`), painter's algo not yet implemented
- **Texture blend**: `texture(u_texture, v_uv / v_uv_w) * vec4(v_colour, 1.0)`

---

## Scene format (.pscene)
nlohmann JSON. Schema lives in `shared/scene_format.hpp`:
```json
{
  "name": "untitled",
  "objects": [
    {
      "name": "cube",
      "mesh_path": "assets/meshes/test.obj",
      "texture_path": "assets/textures/test.tga",
      "position": [0, 0, 0],
      "rotation": [0, 0, 0],
      "scale": [1, 1, 1],
      "collision": "box"
    }
  ]
}
```
`collision` values: `none` | `box` | `mesh` | `convex` (box = AABB auto-fit)

---

## Milestone status

### ✅ Milestone 0 — triangle
SDL2 window, GLAD (Wayland-safe), Dear ImGui overlay, PSX FBO pipeline,
vertex snap shader, 320×240 nearest upscale.

### ✅ Milestone 1 — OBJ + camera
Manual OBJ loader (v/vt/f, fan triangulation). Free-look camera (WASD +
mouse, SDL relative mode). MVP uniform. Assets copied by CMake post-build.

### ✅ Milestone 2 — textures
Manual TGA loader (type-2, 24-bit BGR). Programmatic 64×64 checkerboard
generated at startup. Affine UV warp. Vertex layout updated to 8 floats.
`u_use_texture` ImGui toggle.

### 🔲 Milestone 3 — Level Editor MVP
**Target: `psx_editor` becomes a usable level editor for non-coders.**

Layout inspired by Blender (simplified):
```
┌─────────────────────────┬──────────────┐
│                         │  Outliner    │
│    3D Viewport          │  (hierarchy) │
│    PSX renderer         ├──────────────┤
│    preview              │  Inspector   │
│                         │  (properties)│
└─────────────────────────┴──────────────┘
```

Panels:
- **Viewport**: PSX renderer preview, middle-mouse orbit, scroll zoom,
  G to grab/move selected object, gizmo on selected object
- **Outliner**: flat list of scene objects, click to select,
  eye icon to toggle visibility, + button to add object
- **Inspector**: Object tab — name, mesh path (file picker), texture path
  (file picker), transform (pos/rot/scale with drag fields),
  collision type dropdown, instance_count for array mesh

Interaction model (Blender-inspired, simplified):
- Middle mouse = orbit camera
- Scroll = zoom
- G = grab (move) selected object on XZ plane
- Escape = deselect / cancel operation
- Del = delete selected object
- Ctrl+S = save .pscene
- Ctrl+O = open .pscene

File I/O:
- Save/load `.pscene` via nlohmann JSON
- Asset paths stored relative to project root

Implementation order (do one step at a time, build between each):
1. Dockspace + empty panels (just labels)
2. Viewport panel with PSX renderer + orbit camera
3. Outliner panel with hardcoded test objects
4. Inspector panel with transform fields
5. Add/delete objects
6. Save/load .pscene
7. File pickers for mesh/texture
8. Gizmo on selected object

### 🔲 Milestone 4 — Physics / Collision
- `SceneObject` collision field: `none` | `box` | `mesh` | `convex`
- Box: AABB auto-fit from mesh bounds (default)
- Mesh: exact triangle collision (level geometry)
- Convex: simplified hull (dynamic objects)
- Editor: wireframe overlay — green=box, yellow=convex, red=mesh
- Engine: AABB/ray tests at runtime, no physics lib required
- Hull data embedded in `.pscene`

---

## Platform
- Arch Linux, Sway/Wayland — no XWayland, no GLX
- ThinkPad T480, Intel UHD 620 (KBL GT2), Mesa 26.0.6
- `$TERMINAL` must be unset when launching Claude Code (`TERMINAL= claude`)
  otherwise Sway intercepts new shells as Alacritty windows
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

## ImGui API notes
- `DockSpaceOverViewport` signature: `DockSpaceOverViewport(0, ImGui::GetMainViewport())`
  — the docking branch requires an explicit `dockspace_id` as first argument

## Milestone 3 progress

### ✅ Step 1 — dockspace + empty panels
Three dockable ImGui panels: Viewport, Outliner, Inspector.

### ✅ Step 2 — viewport panel + orbit camera
PSX FBO rendered inside Viewport panel via ImGui::Image (Y-flipped).
OrbitCamera: MMB drag = orbit, Shift+MMB = pan, scroll = zoom.
Numpad 1/3/7 = front/side/top snap, Numpad 5 = ortho/persp toggle.
SDL cursor feedback: crosshair=orbit, sizeall=pan, arrow=idle.
Left-click reserved for object selection (step 3+).

### ✅ Step 3 — Outliner panel
Flat list of scene objects, click to select/deselect, visibility toggle [v]/[ ].
Note: emoji literals (u8"...") are incompatible with const char* in C++20 — use plain ASCII strings for ImGui labels.

### ✅ Step 4 — Inspector panel
Name (editable), Position/Rotation/Scale (DragFloat3). Edits SceneObject directly.
Transform fields not yet connected to viewport render — wired in step 6+.

### ✅ Step 5 — add/delete objects
"+" button adds default SceneObject (auto-selected). Del key deletes selected.
Post-deletion selection: previous object or -1 if list empty.
SceneObject now has mesh_path and texture_path fields with defaults.

### ✅ Step 6 — save/load .pscene + menubar
Ctrl+S / Ctrl+O save and load scene.pscene (nlohmann JSON).
SceneObject has collision field (default "box").
Status message in Outliner fades after 2 seconds.
Menubar: File (New/Open/Save/Quit), Edit (Add/Delete), View (snap + ortho toggle).
Dockspace placed after BeginMainMenuBar so panels don't overlap the bar.

### ✅ Step 7 — file pickers
"..." button opens ImGui modal popup listing files from assets/meshes/ (.obj)
and assets/textures/ (.tga/.png/.bmp). Click file → path updates, popup closes.
Note: EndPopupModal does not exist — use EndPopup after BeginPopupModal.

### ✅ Milestone 3 polish
- Window title: "psx-editor — filename *" dirty indicator
- Undo/redo: snapshot-based, max 50, Ctrl+Z/Y/Shift+Z
- Ctrl+D duplicates selected object (name + " (copy)", offset 0.5/0/0.5)
- Edit menu: Duplicate, Undo, Redo (grayed when unavailable)
- SDL_Keymod cast fix: (SDL_Keymod)ev.key.keysym.mod
