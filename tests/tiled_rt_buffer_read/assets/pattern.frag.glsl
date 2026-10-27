#version 450

// Per-pixel RGBA8 pattern. Must match pixel_pattern() in main.c.

layout(location = 0) out vec4 out_color;

void main() {
    uvec2 p = uvec2(gl_FragCoord.xy);
    uint r = (p.x * 3u + p.y) & 255u;
    uint g = (p.y * 5u + 7u) & 255u;
    uint b = (p.x ^ p.y) & 255u;
    out_color = vec4(float(r), float(g), float(b), 255.0) / 255.0;
}
