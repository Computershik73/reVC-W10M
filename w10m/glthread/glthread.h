// OpenGL ES on a thread of its own: see glthread.cpp.
#ifndef REVC_W10M_GLTHREAD_H
#define REVC_W10M_GLTHREAD_H

#include <stdint.h>

// The OpenGL ES context that is current on the calling thread goes to a new thread, the
// drawing thread, and from here on every OpenGL call made through glad's pointers is made
// there. The calling thread is then the only one that may call OpenGL. False, with the reason
// logged, and nothing changed, if that could not be done.
bool glt_start(void (*log)(const char* text));
// The other way: everything asked for is done, the drawing thread ends, the context is the
// calling thread's again.
void glt_stop(void);
bool glt_on(void);

// What is not an OpenGL call through glad and still has to happen on the drawing thread
// (EGL, a function looked up by name). post: in its turn, the caller goes on. call: in its
// turn, and the caller waits for it. The function must not call OpenGL through glad itself.
void glt_post(void (*fn)(void*), void* arg);
void glt_call(void (*fn)(void*), void* arg);
// The end of a frame: `show` (the swap) in its turn. The caller is held here while the frame
// before this one has not been shown yet: it is never more than a frame ahead of the picture.
void glt_frame(void (*show)(void*), void* arg);

// Since it was last asked.
struct GltStats {
    double drawingMs;     // the drawing thread at work
    double frameWaitMs;   // the caller held back at the ends of frames
    double answerWaitMs;  // the caller waiting for calls that have an answer
    uint32_t calls;       // made by the drawing thread
    uint32_t answers;     // calls the caller waited for
    uint32_t timesFull;   // the caller found the queue full
    uint32_t sleeps;      // times the drawing thread ran out of calls and slept
    // with glt_timing(true), the drawing thread's time by kind of call: draws from buffers,
    // draws from the caller's memory, textures handed over, buffers filled
    double kindMs[4];
    uint32_t kindCalls[4];
    double longDrawMs[2];  // of the draws of each kind, those that took over a millisecond each
    uint32_t longDraws[2];
    double memoryDrawKB;   // what the draws from memory brought with them
};
void glt_stats(GltStats* stats);
// Whether the drawing thread times its draws and what is handed over to it (two readings of
// the clock for each): for a log of where its time goes.
void glt_timing(bool on);

#endif
