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

#ifdef __cplusplus
}
#endif

#endif
