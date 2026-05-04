#version 330 core

in  vec2 v_uv;
in  vec3 v_color;
out vec4 frag_color;

uniform sampler2D u_texture;
uniform int       u_use_texture;

void main() {
    if (u_use_texture != 0)
        frag_color = texture(u_texture, v_uv) * vec4(v_color, 1.0);
    else
        frag_color = vec4(v_color, 1.0);
}
