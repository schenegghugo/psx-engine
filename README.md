# psx-engine

A C++ PSX-aesthetic 3D engine with its own level editor — built as an experiment in the Claude Code workflow.

![status](https://img.shields.io/badge/status-milestone%204%20complete-green)
![platform](https://img.shields.io/badge/platform-linux%20%7C%20windows-blue)
![language](https://img.shields.io/badge/C%2B%2B-20-orange)

## What this is

Two cross-platform C++ executables sharing a renderer:

- **`psx_engine`** — runtime, loads `.pscene` files, renders with the PSX aesthetic
- **`psx_editor`** — Blender-inspired level editor for non-coders to build scenes

The PSX look comes from honest emulation of the original hardware constraints: vertex snapping (the wobble), affine texture warping (the swim), 320×240 internal framebuffer upscaled with nearest-neighbour, and a limited geometry budget.

## Milestone 4: Scene Tree Architecture
Transitioned from a flat object list to a **hierarchical parent/child scene graph**, establishing the foundation for engine-ready spatial organization.



### Core Upgrades
* **Schema Redesign:** Replaced flat arrays with `psx::Scene` nodes. Nodes now feature local transforms and data-only JSON components (Mesh, Camera, Light, etc.).
* **Recursive Outliner:** Implemented a tree view with drag-and-drop reparenting, cycle prevention, and a type-picker dropdown (Empty, Mesh, Camera, Light, Player).
* **Transform Hierarchy:** Nodes store **local transforms**. World matrices are computed via parent-chain propagation (e.g., moving a Player moves their child Camera/Collision rig).
* **Robust Rotations:** Migrated to `glm::quat` to eliminate gimbal lock with "preferred Euler" storage for UI consistency.
* **Visual Gizmos:** Added wireframe indicators for non-mesh nodes (Cameras, Lights) with depth-test disabled for constant visibility.
* **Manipulation:** Added Global/Local coordinate toggles, precision mode (Shift), and snap functionality (Ctrl).

## Aesthetic features
- **Vertex snap** — screen-space coordinate quantisation
- **Affine UV warp** — perspective-incorrect texture interpolation
- **320×240 FBO** with `GL_NEAREST` upscale
- **Texture × vertex colour** blend
- Geometry budget validation

## Tech stack

| Concern | Library |
|---|---|
| Window / input | SDL2 |
| GL loader | GLAD (vendored) |
| GL | OpenGL 3.3 core |
| UI | Dear ImGui (docking) |
| Gizmo | ImGuizmo |
| Math | GLM |
| ECS | EnTT |
| JSON | nlohmann_json |
| Build | CMake + Ninja, vcpkg |

## Building

### Linux (Arch)
```bash
git clone git@github.com:schenegghugo/psx-engine.git
cd psx-engine
bash psxSetup.sh psx-engine
bash build-linux.sh
```

### Windows (cross-compile)
```bash
bash build-windows.sh
```

## Project structure

```
engine/                 runtime executable
editor/                 level editor executable
shared/                 types used by both (scene_format.hpp)
vendor/                 third-party dependencies
assets/                 meshes/textures
```

## Milestones

- ✅ **Milestone 0** — PSX triangle, vertex snap, 320×240 nearest upscale
- ✅ **Milestone 1** — OBJ loader, free-look camera, MVP pipeline
- ✅ **Milestone 2** — TGA textures, affine UV warp, programmatic checkerboard
- ✅ **Milestone 3** — Full level editor (viewport, outliner, inspector, gizmo)
- ✅ **Milestone 4** — Hierarchical scene tree, parent/child transforms, quaternion rotations
- 🔲 **Milestone 5** — Physics: AABB/mesh/convex collision
- 🔲 **Milestone 6** — Asset pipeline (texture quantisation, binary mesh format)
- 🔲 **Milestone 7** — Audio, gameplay scripting hooks

## About the workflow
This project is an experiment in pair-programming with Claude Code. Full session logs are maintained in `CLAUDE.md`.

## License
MIT
