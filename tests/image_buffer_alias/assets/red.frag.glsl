#version 450

// Writes only the red channel's value; the draw's render-target mask keeps
// green, blue and alpha. Must match RED_VALUE in main.c.

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(90.0 / 255.0, 0.0, 0.0, 0.0);
}
