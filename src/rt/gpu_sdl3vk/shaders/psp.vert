#version 450
/* PSP GE vertex stage, Phase 1. Vertices arrive ALREADY transformed and projected by the
 * (reference-correct) CPU T&L in ge.c: x/y are screen pixels snapped to the PSP's 28.4
 * subpixel grid, z is the 16-bit PSP depth (0..65535), rw is 1/clipw (1.0 in through
 * mode). All varyings are noperspective: the software rasterizer interpolates u*rw, v*rw
 * and rw AFFINELY in screen space and divides per pixel, which this reproduces exactly.
 * The render target is the full 512x272 PSP framebuffer stride. */
layout(location = 0) in vec4 in_pos;    /* x px, y px, z 0..65535, rw */
layout(location = 1) in vec2 in_uv;     /* u*rw, v*rw texel coords (raw u,v in through mode) */
layout(location = 2) in float in_fog;   /* per-vertex fog factor, 1 = no fog */
layout(location = 3) in vec4 in_color;  /* UNORM8 RGBA */

layout(location = 0) noperspective out vec2  v_uv;
layout(location = 1) noperspective out float v_rw;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) noperspective out vec4  v_color;

void main() {
    gl_Position = vec4(in_pos.x * (1.0 / 256.0) - 1.0,
                       in_pos.y * (1.0 / 136.0) - 1.0,
                       in_pos.z * (1.0 / 65535.0),
                       1.0);
    v_uv    = in_uv;
    v_rw    = in_pos.w;
    v_fog   = in_fog;
    v_color = in_color;
}
