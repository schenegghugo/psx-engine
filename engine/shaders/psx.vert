#version 330 core

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec3 a_color;

out vec2 v_uv;
out vec3 v_color;

uniform mat4  u_mvp;
uniform float u_snap_resolution; // 0 = disabled, e.g. 60 = heavy snap

void main() {
    vec4 clip = u_mvp * vec4(a_pos, 1.0);

    // PSX vertex snapping: snap NDC to a coarse grid
    if (u_snap_resolution > 0.0) {
        vec2 half_res = vec2(u_snap_resolution * 0.5);
        clip.xy = clip.xy / clip.w;
        clip.xy = floor(clip.xy * half_res + 0.5) / half_res;
        clip.xy *= clip.w;
    }

    gl_Position = clip;

    // Affine texture warp: multiply UVs by w before the rasterizer.
    // OpenGL's perspective-correct division then cancels the w, leaving
    // UVs that interpolate linearly in screen space — the PSX swim effect.
    v_uv    = a_uv * clip.w;
    v_color = a_color;
}
