#version 330 core

in vec2 vs_tex;
in vec2 vs_vertex;

out vec2 fs_tex;

void main() {
    fs_tex = vs_tex;
    gl_Position = vec4(vs_vertex, 0.0f, 1.0f);
}

