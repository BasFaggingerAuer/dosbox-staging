#version 330 core

uniform sampler2D framebuffer;
uniform vec2 window_size;
uniform vec2 framebuffer_size;

in vec2 fs_tex;
out vec4 fragment;

void main() {
    fragment = vec4(texture(framebuffer, fs_tex).xyz, 1.0f);
}

