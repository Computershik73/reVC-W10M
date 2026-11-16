// The drawing thread of the OpenGL build (w10m/glthread) carries OpenGL's calls from the game's
// thread to one of its own. The Direct3D 11 renderer makes its calls where the game asks for
// them, so there is no such thread: this is glthread.h for the code that asks whether there is
// one (the GLFW layer, the game's skeleton).
#include "glthread.h"

#include <string.h>

bool glt_start(void (*)(const char*)) { return false; }
void glt_stop(void) {}
bool glt_on(void) { return false; }

void glt_post(void (*fn)(void*), void* arg) { fn(arg); }
void glt_call(void (*fn)(void*), void* arg) { fn(arg); }
void glt_frame(void (*show)(void*), void* arg) { show(arg); }

void glt_stats(GltStats* stats) { memset(stats, 0, sizeof(*stats)); }
void glt_timing(bool) {}
