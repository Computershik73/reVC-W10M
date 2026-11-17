// Between the Direct3D 11 renderer (device.cpp) and GLFW's API for the port (w10m/glfw), which
// is the window, its size and the frame's end for both of them.
#ifndef RWD3D11_HOST_H
#define RWD3D11_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

// The frame is shown: after the next vertical blank where interval is 1, at once where 0.
void rwd3d11_show(int interval);
// The picture as it is now, as RGBA with its bottom row first. False if there is none of
// that size.
bool rwd3d11_read_back(int width, int height, unsigned char *rgba);
// The panel has another size: what is drawn into is made anew at it.
void rwd3d11_resize(int width, int height);
// Direct3D's feature level once the renderer has started, as 93 for 9_3 or 110 for 11_0; 0
// before.
int rwd3d11_feature_level(void);
// REVC_PERF: where the renderer's own time went in the frames since the last call, as text;
// the number of characters.
int rwd3d11_perf_line(char *line, int size, int frames);
// What immediate mode draws in two dimensions from now on (on: 1) lies behind everything
// drawn so far, however it asks to be drawn: the sky, drawn after the world (immed.cpp).
void rwd3d11_draw_behind(int on);
// What the GPU is asked for, counted (measure.cpp). The triangles since the renderer started.
unsigned long long rwd3d11_triangles(void);
// REVC_D3D_PIXELS: 0 if the pixels drawn are not counted, 1 if they are, 2 if every pixel a
// triangle covers is (no depth test).
int rwd3d11_counts_pixels(void);
// From here on they are counted apart from those before: the number of this place in the frame.
int rwd3d11_pixel_mark(void);
// The pixels drawn between two such places of the frame shown last, as so many pictures'
// worth; to < 0: up to the frame's end.
float rwd3d11_screens_between(int from, int to);

#ifdef __cplusplus
}
#endif

#endif
