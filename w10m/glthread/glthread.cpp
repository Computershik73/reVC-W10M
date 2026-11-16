// OpenGL ES on a thread of its own.
//
// On a phone whose GPU stops at Direct3D level 9 a draw is a hundred microseconds of ANGLE and
// of the driver on the thread that calls it, and the game has three to nine hundred of them a
// frame: two thirds of the game thread's frame went there (w10m/README.md, "The slow phone").
// Here the calls are written into a queue instead and a second thread, the drawing thread,
// makes them, a frame behind at most: the game's own work and the driver's are then on two of
// the phone's four cores.
//
// librw and the game call OpenGL through glad's table of pointers. glt_start() takes the
// context from the calling thread, gives it to the drawing thread and puts a function of this
// file in the place of every pointer; glt_stop() undoes it. The functions are of three kinds:
//
//   queued       no answer and nothing but numbers for arguments: the arguments are copied
//                into the queue, the caller goes on
//   queued with  uniforms, vertices, pixels, names to delete: copied with what the pointer
//   their data   points at (into the queue, or onto the heap when that is long). A draw from
//                the caller's memory is one of these: its vertices and indices go with it
//   waited for   anything else (an answer, a pointer to fill, a string): the caller waits
//                until the drawing thread has made every call before it and this one. That is
//                always right and never fast; nothing the game does in every frame is of
//                this kind, and glt_stats() counts them
//
// calls.inc has the first and the third kind for every function glad knows; it is made from
// glad.h by tools/gen_glthread.py. The second kind is below, by hand.
//
// One thread writes the queue (the game's), one reads it. A command is in one piece: where
// the ring's end is too near for it, the rest is skipped.
#include <windows.h>
#include <process.h>

#include <EGL/egl.h>

#include "glad/glad.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <type_traits>

#include "glthread.h"

namespace {

struct Cmd {
    void (*run)(Cmd*);
    uint32_t size;  // with everything that follows it; a multiple of 16
};

const uint32_t kRing = 1u << 21;     // the queue, in bytes: a frame is a tenth of it
const uint32_t kInline = 64 * 1024;  // data up to this long goes into the queue itself
const size_t kHeapAhead = 8u << 20;  // longer data waits on the heap: no more than this much

uint8_t* gRing;
std::atomic<uint32_t> gHead{0}, gTail{0};  // bytes written and bytes run; they only grow, and wrap as one
std::atomic<bool> gAsleep{false};          // the drawing thread has nothing to do and waits for gWake
std::atomic<size_t> gHeap{0};
std::atomic<uint32_t> gFramesShown{0};
std::atomic<LONGLONG> gBusyTicks{0};
std::atomic<uint32_t> gSleeps{0};  // times the drawing thread ran out of calls and slept
std::atomic<uint32_t> gCalls{0};
HANDLE gWake, gAnswer, gShown, gThread;

// the calling thread's
bool gOn;
DWORD gCaller;
uint32_t gSeenTail, gFramesSent;
uint32_t gAnswers, gTimesFull;
LONGLONG gAnswerTicks, gFrameTicks;
void (*gLog)(const char*);
EGLDisplay gDisplay;
EGLSurface gDraw, gRead;
EGLContext gContext;
GLint gUnpackAlignment = 4;
GLuint gUnpackBuffer;
bool gUnpackOdd;

// the drawing thread's
bool gQuit;
LONGLONG gBusyFrom;

LONGLONG Ticks() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double Milliseconds(LONGLONG ticks) {
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    return (double)ticks * 1000.0 / (double)frequency.QuadPart;
}

void Log(const char* text) {
    if (gLog) gLog(text);
}

inline uint32_t Round(size_t bytes) { return (uint32_t)((bytes + 15) & ~(size_t)15); }

// ===[ The queue, from the calling thread's side ]===

void Publish(Cmd* c) {
    gHead.store(gHead.load(std::memory_order_relaxed) + c->size, std::memory_order_seq_cst);
    // the drawing thread says it sleeps before its last look at the head, this side looks
    // after moving the head: one of the two sees the other
    if (gAsleep.load(std::memory_order_seq_cst)) SetEvent(gWake);
}

void WaitForRoom(uint32_t head, uint32_t bytes) {
    // head - tail is what has not been run yet
    if (head + bytes - gSeenTail <= kRing) return;
    for (int tries = 0;; tries++) {
        gSeenTail = gTail.load(std::memory_order_acquire);
        if (head + bytes - gSeenTail <= kRing) return;
        if (tries == 0) gTimesFull++;
        Sleep(tries < 50 ? 0 : 1);
    }
}

Cmd* Reserve(uint32_t bytes) {
    if (GetCurrentThreadId() != gCaller) {
        static bool noted;
        if (!noted) Log("glthread: OpenGL is called from a thread that is not the one that started the drawing thread");
        noted = true;
    }
    uint32_t head = gHead.load(std::memory_order_relaxed);
    uint32_t at = head & (kRing - 1);
    if (kRing - at < bytes) {
        uint32_t rest = kRing - at;
        WaitForRoom(head, rest);
        Cmd* skip = (Cmd*)(gRing + at);
        skip->run = [](Cmd*) {};
        skip->size = rest;
        Publish(skip);
        head += rest;
        at = 0;
    }
    WaitForRoom(head, bytes);
    Cmd* c = (Cmd*)(gRing + at);
    c->size = bytes;
    return c;
}

// The call is made on the drawing thread, after everything queued before it, and the caller
// waits for it.
template <class F>
void Wait(F&& f) {
    typedef typename std::remove_reference<F>::type Function;
    struct C {
        Cmd cmd;
        Function* f;
    };
    C* c = (C*)Reserve(Round(sizeof(C)));
    c->cmd.run = [](Cmd* x) {
        (*((C*)x)->f)();
        SetEvent(gAnswer);
    };
    c->f = &f;
    LONGLONG from = Ticks();
    Publish(&c->cmd);
    WaitForSingleObjectEx(gAnswer, INFINITE, FALSE);
    gAnswers++;
    gAnswerTicks += Ticks() - from;
}

// ===[ Where the drawing thread's time goes (glt_timing) ]===

enum { kDraws, kMemoryDraws, kTextures, kBuffers, kKinds };
std::atomic<bool> gTiming{false};
std::atomic<LONGLONG> gKindTicks[kKinds];
std::atomic<uint32_t> gKindCalls[kKinds];
std::atomic<LONGLONG> gLongDrawTicks[2];
std::atomic<uint32_t> gLongDraws[2];
std::atomic<uint32_t> gMemoryDrawBytes{0};

// In a command of the drawing thread: the time of what follows it there goes to its kind.
struct Timed {
    int kind;
    LONGLONG from;
    explicit Timed(int k) : kind(k), from(gTiming.load(std::memory_order_relaxed) ? Ticks() : 0) {}
    ~Timed() {
        if (!from) return;
        static LARGE_INTEGER frequency;
        if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
        LONGLONG took = Ticks() - from;
        gKindTicks[kind].fetch_add(took, std::memory_order_relaxed);
        gKindCalls[kind].fetch_add(1, std::memory_order_relaxed);
        if (kind <= kMemoryDraws && took * 1000 > frequency.QuadPart) {
            gLongDrawTicks[kind].fetch_add(took, std::memory_order_relaxed);
            gLongDraws[kind].fetch_add(1, std::memory_order_relaxed);
        }
    }
};

// A command with data: in the queue right behind it, or on the heap.
struct DataCmd {
    Cmd cmd;
    void* heap;
    uint32_t bytes;
    uint32_t at;  // where the data is, from the command's start; 0 with no heap: there is none
};

inline const void* Data(DataCmd* d) { return d->heap ? d->heap : d->at ? (const uint8_t*)d + d->at : nullptr; }

inline void Done(DataCmd* d) {
    if (!d->heap) return;
    free(d->heap);
    gHeap.fetch_sub(d->bytes, std::memory_order_relaxed);
}

// C begins with a DataCmd called d. *space is where its bytes of data are to be written (null
// for none).
template <class C>
C* ReserveSpace(size_t bytes, uint8_t** space) {
    uint32_t fixed = Round(sizeof(C));
    C* c;
    if (!bytes) {
        c = (C*)Reserve(fixed);
        c->d.heap = nullptr;
        c->d.bytes = c->d.at = 0;
        *space = nullptr;
    } else if (bytes <= kInline) {
        c = (C*)Reserve(fixed + Round(bytes));
        c->d.heap = nullptr;
        c->d.bytes = (uint32_t)bytes;
        c->d.at = fixed;
        *space = (uint8_t*)c + fixed;
    } else {
        // a level being loaded hands over textures faster than they are taken: not without end
        if (gHeap.load(std::memory_order_relaxed) > kHeapAhead) Wait([] {});
        void* copy = malloc(bytes);
        if (!copy) {
            Wait([] {});  // with everything taken, what was waiting is free again
            copy = malloc(bytes);
        }
        if (!copy) {
            Log("glthread: no memory for the data of an OpenGL call");
            abort();
        }
        gHeap.fetch_add(bytes, std::memory_order_relaxed);
        c = (C*)Reserve(fixed);
        c->d.heap = copy;
        c->d.bytes = (uint32_t)bytes;
        c->d.at = 0;
        *space = (uint8_t*)copy;
    }
    return c;
}

// The same, with the data copied from where the caller has it.
template <class C>
C* ReserveWith(const void* data, size_t bytes) {
    uint8_t* space;
    C* c = ReserveSpace<C>(data ? bytes : 0, &space);
    if (space) memcpy(space, data, bytes);
    return c;
}

// What this side has to know of the context's state to tell a pointer into the caller's
// memory from an offset into a buffer. The default vertex array's (number 0, the only one
// that may point into memory), followed as the calls go by.
struct FromMemory {
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    const uint8_t* pointer;
};
FromMemory gFromMemory[16];
uint32_t gFromMemoryMask;  // its attributes that point into memory
uint32_t gEnabledMask;     // its attributes that are enabled
GLuint gElementBuffer;     // its element array buffer
GLuint gArrayBuffer;       // the array buffer, which is the context's
GLuint gVertexArray;       // the vertex array bound now

// ===[ The functions that go into glad's table ]===

#include "calls.inc"

#define UNIFORM(name, type, each)                                                                     \
    void APIENTRY w_##name(GLint location, GLsizei count, const type* value) {                        \
        struct C {                                                                                    \
            DataCmd d;                                                                                \
            GLint location;                                                                           \
            GLsizei count;                                                                            \
        };                                                                                            \
        C* c = ReserveWith<C>(value, count > 0 ? (size_t)count * each * sizeof(type) : 0);            \
        c->d.cmd.run = [](Cmd* x) {                                                                   \
            C* c = (C*)x;                                                                             \
            r_##name(c->location, c->count, (const type*)Data(&c->d));                                \
            Done(&c->d);                                                                              \
        };                                                                                            \
        c->location = location;                                                                       \
        c->count = count;                                                                             \
        Publish(&c->d.cmd);                                                                           \
    }
UNIFORM(glUniform1fv, GLfloat, 1)
UNIFORM(glUniform2fv, GLfloat, 2)
UNIFORM(glUniform3fv, GLfloat, 3)
UNIFORM(glUniform4fv, GLfloat, 4)
UNIFORM(glUniform1iv, GLint, 1)
UNIFORM(glUniform2iv, GLint, 2)
UNIFORM(glUniform3iv, GLint, 3)
UNIFORM(glUniform4iv, GLint, 4)
#undef UNIFORM

#define MATRIX(name, each)                                                                            \
    void APIENTRY w_##name(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) { \
        struct C {                                                                                    \
            DataCmd d;                                                                                \
            GLint location;                                                                           \
            GLsizei count;                                                                            \
            GLboolean transpose;                                                                      \
        };                                                                                            \
        C* c = ReserveWith<C>(value, count > 0 ? (size_t)count * each * sizeof(GLfloat) : 0);         \
        c->d.cmd.run = [](Cmd* x) {                                                                   \
            C* c = (C*)x;                                                                             \
            r_##name(c->location, c->count, c->transpose, (const GLfloat*)Data(&c->d));               \
            Done(&c->d);                                                                              \
        };                                                                                            \
        c->location = location;                                                                       \
        c->count = count;                                                                             \
        c->transpose = transpose;                                                                     \
        Publish(&c->d.cmd);                                                                           \
    }
MATRIX(glUniformMatrix2fv, 4)
MATRIX(glUniformMatrix3fv, 9)
MATRIX(glUniformMatrix4fv, 16)
#undef MATRIX

void APIENTRY w_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    struct C {
        DataCmd d;
        GLenum target;
        GLsizeiptr size;
        GLenum usage;
    };
    C* c = ReserveWith<C>(data, size > 0 ? (size_t)size : 0);
    c->d.cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        Timed timed(kBuffers);
        r_glBufferData(c->target, c->size, Data(&c->d), c->usage);
        Done(&c->d);
    };
    c->target = target;
    c->size = size;
    c->usage = usage;
    Publish(&c->d.cmd);
}

void APIENTRY w_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    struct C {
        DataCmd d;
        GLenum target;
        GLintptr offset;
        GLsizeiptr size;
    };
    C* c = ReserveWith<C>(data, size > 0 ? (size_t)size : 0);
    c->d.cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        Timed timed(kBuffers);
        r_glBufferSubData(c->target, c->offset, c->size, Data(&c->d));
        Done(&c->d);
    };
    c->target = target;
    c->offset = offset;
    c->size = size;
    Publish(&c->d.cmd);
}

#define DELETE_NAMES(function, name)                                                                  \
    void APIENTRY function(GLsizei n, const GLuint* names) {                                          \
        struct C {                                                                                    \
            DataCmd d;                                                                                \
            GLsizei n;                                                                                \
        };                                                                                            \
        C* c = ReserveWith<C>(names, n > 0 ? (size_t)n * sizeof(GLuint) : 0);                         \
        c->d.cmd.run = [](Cmd* x) {                                                                   \
            C* c = (C*)x;                                                                             \
            r_##name(c->n, (const GLuint*)Data(&c->d));                                               \
            Done(&c->d);                                                                              \
        };                                                                                            \
        c->n = n;                                                                                     \
        Publish(&c->d.cmd);                                                                           \
    }
DELETE_NAMES(w_glDeleteTextures, glDeleteTextures)
DELETE_NAMES(w_glDeleteFramebuffers, glDeleteFramebuffers)
DELETE_NAMES(w_glDeleteRenderbuffers, glDeleteRenderbuffers)

// what is deleted while it is bound is not bound any more
DELETE_NAMES(DeleteBuffers, glDeleteBuffers)
void APIENTRY w_glDeleteBuffers(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; names && i < n; i++) {
        if (names[i] == gArrayBuffer) gArrayBuffer = 0;
        if (names[i] == gElementBuffer) gElementBuffer = 0;
        if (names[i] == gUnpackBuffer) gUnpackBuffer = 0;
    }
    DeleteBuffers(n, names);
}

DELETE_NAMES(DeleteVertexArrays, glDeleteVertexArrays)
void APIENTRY w_glDeleteVertexArrays(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; names && i < n; i++)
        if (names[i] == gVertexArray) gVertexArray = 0;
    DeleteVertexArrays(n, names);
}
#undef DELETE_NAMES

// Names are only numbers set aside: a model that comes in while the game is driven makes a
// few buffers and a vertex array, and would wait for the drawing thread each time. They are
// made by the dozen and handed out from here.
struct Names {
    GLuint names[48];
    int left;
};
Names gBuffers, gTextures, gVertexArrays;

#define GEN_NAMES(name, pool)                                                                         \
    void APIENTRY w_##name(GLsizei n, GLuint* names) {                                                \
        for (GLsizei i = 0; i < n; i++) {                                                             \
            if (pool.left == 0) {                                                                     \
                Wait([] { r_##name((GLsizei)(sizeof(pool.names) / sizeof(GLuint)), pool.names); });   \
                pool.left = (int)(sizeof(pool.names) / sizeof(GLuint));                               \
            }                                                                                         \
            names[i] = pool.names[--pool.left];                                                       \
        }                                                                                             \
    }
GEN_NAMES(glGenBuffers, gBuffers)
GEN_NAMES(glGenTextures, gTextures)
GEN_NAMES(glGenVertexArrays, gVertexArrays)
#undef GEN_NAMES

void APIENTRY w_glPixelStorei(GLenum pname, GLint param) {
    if (pname == GL_UNPACK_ALIGNMENT)
        gUnpackAlignment = param;
    else if (pname == GL_UNPACK_ROW_LENGTH || pname == GL_UNPACK_SKIP_ROWS || pname == GL_UNPACK_SKIP_PIXELS ||
             pname == GL_UNPACK_IMAGE_HEIGHT || pname == GL_UNPACK_SKIP_IMAGES)
        gUnpackOdd = gUnpackOdd || param != 0;
    struct C {
        Cmd cmd;
        GLenum pname;
        GLint param;
    };
    C* c = (C*)Reserve(Round(sizeof(C)));
    c->cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        r_glPixelStorei(c->pname, c->param);
    };
    c->pname = pname;
    c->param = param;
    Publish(&c->cmd);
}

void APIENTRY w_glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER)
        gArrayBuffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        if (gVertexArray == 0) gElementBuffer = buffer;  // each vertex array has its own
    } else if (target == GL_PIXEL_UNPACK_BUFFER)
        gUnpackBuffer = buffer;
    struct C {
        Cmd cmd;
        GLenum target;
        GLuint buffer;
    };
    C* c = (C*)Reserve(Round(sizeof(C)));
    c->cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        r_glBindBuffer(c->target, c->buffer);
    };
    c->target = target;
    c->buffer = buffer;
    Publish(&c->cmd);
}

// The bytes glTexImage2D reads for a picture of this size; 0 where that is not known here
// (the caller waits for the call then, which needs no copy).
size_t PixelBytes(GLsizei width, GLsizei height, GLenum format, GLenum type) {
    if (gUnpackBuffer || gUnpackOdd || width <= 0 || height <= 0) return 0;
    size_t pixel = 0;
    switch (type) {
        case GL_UNSIGNED_BYTE:
            switch (format) {
                case GL_RGBA:
                case GL_BGRA: pixel = 4; break;
                case GL_RGB: pixel = 3; break;
                case GL_LUMINANCE_ALPHA:
                case GL_RG: pixel = 2; break;
                case GL_LUMINANCE:
                case GL_ALPHA:
                case GL_RED: pixel = 1; break;
                default: break;
            }
            break;
        case GL_UNSIGNED_SHORT_5_6_5:
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1: pixel = 2; break;
        default: break;
    }
    if (!pixel) return 0;
    size_t row = (size_t)width * pixel;
    size_t alignment = gUnpackAlignment > 0 ? (size_t)gUnpackAlignment : 4;
    size_t padded = (row + alignment - 1) / alignment * alignment;
    return padded * (size_t)(height - 1) + row;  // the last row is not read past its pixels
}

void APIENTRY w_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                             GLint border, GLenum format, GLenum type, const void* pixels) {
    size_t bytes = pixels ? PixelBytes(width, height, format, type) : 0;
    if (pixels && !bytes) {
        Wait([&] { r_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels); });
        return;
    }
    struct C {
        DataCmd d;
        GLenum target;
        GLint level, internalformat;
        GLsizei width, height;
        GLint border;
        GLenum format, type;
    };
    C* c = ReserveWith<C>(pixels, bytes);
    c->d.cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        Timed timed(kTextures);
        r_glTexImage2D(c->target, c->level, c->internalformat, c->width, c->height, c->border, c->format, c->type,
                       Data(&c->d));
        Done(&c->d);
    };
    c->target = target;
    c->level = level;
    c->internalformat = internalformat;
    c->width = width;
    c->height = height;
    c->border = border;
    c->format = format;
    c->type = type;
    Publish(&c->d.cmd);
}

void APIENTRY w_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                GLsizei height, GLenum format, GLenum type, const void* pixels) {
    size_t bytes = pixels ? PixelBytes(width, height, format, type) : 0;
    if (!bytes) {  // nothing to copy by, or a size not known here
        Wait([&] { r_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels); });
        return;
    }
    struct C {
        DataCmd d;
        GLenum target;
        GLint level, xoffset, yoffset;
        GLsizei width, height;
        GLenum format, type;
    };
    C* c = ReserveWith<C>(pixels, bytes);
    c->d.cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        Timed timed(kTextures);
        r_glTexSubImage2D(c->target, c->level, c->xoffset, c->yoffset, c->width, c->height, c->format, c->type,
                          Data(&c->d));
        Done(&c->d);
    };
    c->target = target;
    c->level = level;
    c->xoffset = xoffset;
    c->yoffset = yoffset;
    c->width = width;
    c->height = height;
    c->format = format;
    c->type = type;
    Publish(&c->d.cmd);
}

void APIENTRY w_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                       GLsizei height, GLint border, GLsizei imageSize, const void* data) {
    if (gUnpackBuffer || (data && imageSize <= 0)) {
        Wait([&] { r_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data); });
        return;
    }
    struct C {
        DataCmd d;
        GLenum target;
        GLint level;
        GLenum internalformat;
        GLsizei width, height;
        GLint border;
        GLsizei imageSize;
    };
    C* c = ReserveWith<C>(data, data ? (size_t)imageSize : 0);
    c->d.cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        Timed timed(kTextures);
        r_glCompressedTexImage2D(c->target, c->level, c->internalformat, c->width, c->height, c->border, c->imageSize,
                                 Data(&c->d));
        Done(&c->d);
    };
    c->target = target;
    c->level = level;
    c->internalformat = internalformat;
    c->width = width;
    c->height = height;
    c->border = border;
    c->imageSize = imageSize;
    Publish(&c->d.cmd);
}

void APIENTRY w_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                          GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
    if (gUnpackBuffer || !data || imageSize <= 0) {
        Wait([&] {
            r_glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
        });
        return;
    }
    struct C {
        DataCmd d;
        GLenum target;
        GLint level, xoffset, yoffset;
        GLsizei width, height;
        GLenum format;
        GLsizei imageSize;
    };
    C* c = ReserveWith<C>(data, (size_t)imageSize);
    c->d.cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        Timed timed(kTextures);
        r_glCompressedTexSubImage2D(c->target, c->level, c->xoffset, c->yoffset, c->width, c->height, c->format,
                                    c->imageSize, Data(&c->d));
        Done(&c->d);
    };
    c->target = target;
    c->level = level;
    c->xoffset = xoffset;
    c->yoffset = yoffset;
    c->width = width;
    c->height = height;
    c->format = format;
    c->imageSize = imageSize;
    Publish(&c->d.cmd);
}

// ===[ Vertices and indices in the caller's memory ]===
// OpenGL ES 2.0 lets the default vertex array point into the caller's memory, and a draw's
// indices be a pointer; librw's immediate mode draws that way where it can (a buffer filled
// anew for every draw is the dearest thing there is on a GPU of Direct3D level 9). The memory
// is the caller's to change as soon as the draw has returned, so here the pointers are not
// passed on when they are set: they are kept, and a draw that uses them is queued as one
// command with the vertices it reaches and its indices copied behind it.

void APIENTRY w_glBindVertexArray(GLuint array) {
    gVertexArray = array;
    struct C {
        Cmd cmd;
        GLuint array;
    };
    C* c = (C*)Reserve(Round(sizeof(C)));
    c->cmd.run = [](Cmd* x) { r_glBindVertexArray(((C*)x)->array); };
    c->array = array;
    Publish(&c->cmd);
}

void APIENTRY w_glEnableVertexAttribArray(GLuint index) {
    if (gVertexArray == 0 && index < 16) gEnabledMask |= 1u << index;
    struct C {
        Cmd cmd;
        GLuint index;
    };
    C* c = (C*)Reserve(Round(sizeof(C)));
    c->cmd.run = [](Cmd* x) { r_glEnableVertexAttribArray(((C*)x)->index); };
    c->index = index;
    Publish(&c->cmd);
}

void APIENTRY w_glDisableVertexAttribArray(GLuint index) {
    if (gVertexArray == 0 && index < 16) gEnabledMask &= ~(1u << index);
    struct C {
        Cmd cmd;
        GLuint index;
    };
    C* c = (C*)Reserve(Round(sizeof(C)));
    c->cmd.run = [](Cmd* x) { r_glDisableVertexAttribArray(((C*)x)->index); };
    c->index = index;
    Publish(&c->cmd);
}

// With a buffer bound the pointer is an offset into it, a number like the others. Without
// one it is the caller's memory (anything under 64 KB is taken for an offset all the same:
// no memory is there).
void APIENTRY w_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                      const void* pointer) {
    if (gVertexArray == 0 && index < 16) {
        if (gArrayBuffer == 0 && (uintptr_t)pointer >= 65536) {
            FromMemory& a = gFromMemory[index];
            a.size = size;
            a.type = type;
            a.normalized = normalized;
            a.stride = stride;
            a.pointer = (const uint8_t*)pointer;
            gFromMemoryMask |= 1u << index;
            return;  // set with the draw that reads it
        }
        gFromMemoryMask &= ~(1u << index);
    }
    struct C {
        Cmd cmd;
        GLuint index;
        GLint size;
        GLenum type;
        GLboolean normalized;
        GLsizei stride;
        const void* pointer;
    };
    C* c = (C*)Reserve(Round(sizeof(C)));
    c->cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        r_glVertexAttribPointer(c->index, c->size, c->type, c->normalized, c->stride, c->pointer);
    };
    c->index = index;
    c->size = size;
    c->type = type;
    c->normalized = normalized;
    c->stride = stride;
    c->pointer = pointer;
    Publish(&c->cmd);
}

size_t TypeBytes(GLenum type) {
    switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
        case GL_HALF_FLOAT: return 2;
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_FLOAT:
        case GL_FIXED: return 4;
        default: return 0;
    }
}

// The attributes of the default vertex array that a draw would read from memory now.
inline uint32_t FromMemoryNow() { return gVertexArray == 0 ? (gFromMemoryMask & gEnabledMask) : 0; }

// Where what is in memory cannot be copied (how far a buffer's indices reach is not known
// here, or a type is not): the draw is made while the caller waits, from the memory itself.
void DrawFromMemoryWaited(uint32_t attribs, GLenum mode, GLint first, GLsizei count, GLenum indexType,
                          const void* indices) {
    Wait([&] {
        if (gArrayBuffer) r_glBindBuffer(GL_ARRAY_BUFFER, 0);
        for (GLuint k = 0; k < 16; k++) {
            if (!(attribs & (1u << k))) continue;
            const FromMemory& a = gFromMemory[k];
            r_glVertexAttribPointer(k, a.size, a.type, a.normalized, a.stride, a.pointer);
        }
        if (gArrayBuffer) r_glBindBuffer(GL_ARRAY_BUFFER, gArrayBuffer);
        if (indexType)
            r_glDrawElements(mode, count, indexType, indices);
        else
            r_glDrawArrays(mode, first, count);
    });
}

// One command: the attributes that are in memory, pointed at the copy; the draw.
// Vertices `lowest` to `highest` are read. Where every enabled attribute is in memory the
// copy begins with vertex `lowest` and the draw counts from there (indices are moved down as
// they are copied); with some in buffers it cannot, and the copy begins with vertex 0.
void DrawFromMemory(uint32_t attribs, GLenum mode, GLint first, GLsizei count, GLenum indexType, const void* indices,
                    uint32_t lowest, uint32_t highest) {
    struct C {
        DataCmd d;
        GLenum mode;
        GLint first;
        GLsizei count;
        GLenum indexType;
        GLuint arrayBuffer;
        uint32_t indicesAt, numAttribs;
        struct {
            GLuint index;
            GLint size;
            GLenum type;
            GLboolean normalized;
            GLsizei stride;
            uint32_t at;
        } attribs[16];
    };
    // attributes that share vertices (one array of structures) are copied as one piece
    struct Piece {
        const uint8_t* from;
        const uint8_t* to;
        GLsizei stride;
        uint32_t at;
    } pieces[16];
    int numPieces = 0, pieceOf[16];
    const uint8_t* startOf[16];
    uint32_t base = (gEnabledMask & ~gFromMemoryMask) ? 0 : lowest;
    for (GLuint k = 0; k < 16; k++) {
        if (!(attribs & (1u << k))) continue;
        const FromMemory& a = gFromMemory[k];
        size_t element = (size_t)a.size * TypeBytes(a.type);
        if (!element) {
            DrawFromMemoryWaited(attribs, mode, first, count, indexType, indices);
            return;
        }
        GLsizei stride = a.stride ? a.stride : (GLsizei)element;
        const uint8_t* from = a.pointer + (size_t)base * stride;
        const uint8_t* to = a.pointer + (size_t)highest * stride + element;
        startOf[k] = from;
        int p;
        for (p = 0; p < numPieces; p++)
            if (pieces[p].stride == stride && from < pieces[p].to + stride && to + stride > pieces[p].from) break;
        if (p == numPieces) {
            pieces[numPieces++] = {from, to, stride, 0};
        } else {
            if (from < pieces[p].from) pieces[p].from = from;
            if (to > pieces[p].to) pieces[p].to = to;
        }
        pieceOf[k] = p;
    }
    size_t bytes = 0;
    for (int p = 0; p < numPieces; p++) {
        pieces[p].at = (uint32_t)bytes;
        bytes += ((size_t)(pieces[p].to - pieces[p].from) + 7) & ~(size_t)7;
    }
    size_t indexBytes = indexType ? TypeBytes(indexType) : 0;
    uint32_t indicesAt = (uint32_t)bytes;
    bytes += indexBytes * (size_t)count;

    uint8_t* space;
    C* c = ReserveSpace<C>(bytes, &space);
    if (gTiming.load(std::memory_order_relaxed)) gMemoryDrawBytes.fetch_add((uint32_t)bytes, std::memory_order_relaxed);
    for (int p = 0; p < numPieces; p++) memcpy(space + pieces[p].at, pieces[p].from, pieces[p].to - pieces[p].from);
    if (indexType == GL_UNSIGNED_SHORT) {
        const uint16_t* in = (const uint16_t*)indices;
        uint16_t* out = (uint16_t*)(space + indicesAt);
        for (GLsizei i = 0; i < count; i++) out[i] = (uint16_t)(in[i] - base);
    } else if (indexType == GL_UNSIGNED_INT) {
        const uint32_t* in = (const uint32_t*)indices;
        uint32_t* out = (uint32_t*)(space + indicesAt);
        for (GLsizei i = 0; i < count; i++) out[i] = in[i] - base;
    } else if (indexType == GL_UNSIGNED_BYTE) {
        const uint8_t* in = (const uint8_t*)indices;
        for (GLsizei i = 0; i < count; i++) space[indicesAt + i] = (uint8_t)(in[i] - base);
    }
    c->mode = mode;
    c->first = indexType ? 0 : first - (GLint)base;
    c->count = count;
    c->indexType = indexType;
    c->arrayBuffer = gArrayBuffer;
    c->indicesAt = indicesAt;
    c->numAttribs = 0;
    for (GLuint k = 0; k < 16; k++) {
        if (!(attribs & (1u << k))) continue;
        const FromMemory& a = gFromMemory[k];
        auto& to = c->attribs[c->numAttribs++];
        to.index = k;
        to.size = a.size;
        to.type = a.type;
        to.normalized = a.normalized;
        to.stride = a.stride;
        to.at = pieces[pieceOf[k]].at + (uint32_t)(startOf[k] - pieces[pieceOf[k]].from);
    }
    c->d.cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        Timed timed(kMemoryDraws);
        const uint8_t* data = (const uint8_t*)Data(&c->d);
        if (c->numAttribs) {
            if (c->arrayBuffer) r_glBindBuffer(GL_ARRAY_BUFFER, 0);
            for (uint32_t i = 0; i < c->numAttribs; i++)
                r_glVertexAttribPointer(c->attribs[i].index, c->attribs[i].size, c->attribs[i].type,
                                        c->attribs[i].normalized, c->attribs[i].stride, data + c->attribs[i].at);
            if (c->arrayBuffer) r_glBindBuffer(GL_ARRAY_BUFFER, c->arrayBuffer);
        }
        if (c->indexType)
            r_glDrawElements(c->mode, c->count, c->indexType, data + c->indicesAt);
        else
            r_glDrawArrays(c->mode, c->first, c->count);
        Done(&c->d);
    };
    Publish(&c->d.cmd);
}

void APIENTRY w_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    uint32_t attribs = FromMemoryNow();
    if (attribs && first >= 0 && count > 0) {
        DrawFromMemory(attribs, mode, first, count, 0, nullptr, (uint32_t)first, (uint32_t)(first + count - 1));
        return;
    }
    struct C {
        Cmd cmd;
        GLenum mode;
        GLint first;
        GLsizei count;
    };
    C* c = (C*)Reserve(Round(sizeof(C)));
    c->cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        Timed timed(kDraws);
        r_glDrawArrays(c->mode, c->first, c->count);
    };
    c->mode = mode;
    c->first = first;
    c->count = count;
    Publish(&c->cmd);
}

void APIENTRY w_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    uint32_t attribs = FromMemoryNow();
    // no element buffer in the default vertex array: the indices are in memory too
    bool indicesInMemory = gVertexArray == 0 && gElementBuffer == 0 && (uintptr_t)indices >= 65536;
    if ((attribs || indicesInMemory) && count > 0) {
        uint32_t lowest = 0xFFFFFFFFu, highest = 0;
        if (indicesInMemory && type == GL_UNSIGNED_SHORT) {
            const uint16_t* in = (const uint16_t*)indices;
            for (GLsizei i = 0; i < count; i++) {
                if (in[i] < lowest) lowest = in[i];
                if (in[i] > highest) highest = in[i];
            }
        } else if (indicesInMemory && type == GL_UNSIGNED_INT) {
            const uint32_t* in = (const uint32_t*)indices;
            for (GLsizei i = 0; i < count; i++) {
                if (in[i] < lowest) lowest = in[i];
                if (in[i] > highest) highest = in[i];
            }
        } else if (indicesInMemory && type == GL_UNSIGNED_BYTE) {
            const uint8_t* in = (const uint8_t*)indices;
            for (GLsizei i = 0; i < count; i++) {
                if (in[i] < lowest) lowest = in[i];
                if (in[i] > highest) highest = in[i];
            }
        } else {
            DrawFromMemoryWaited(attribs, mode, 0, count, type, indices);
            return;
        }
        DrawFromMemory(attribs, mode, 0, count, type, indices, lowest, highest);
        return;
    }
    struct C {
        Cmd cmd;
        GLenum mode;
        GLsizei count;
        GLenum type;
        const void* indices;
    };
    C* c = (C*)Reserve(Round(sizeof(C)));
    c->cmd.run = [](Cmd* x) {
        C* c = (C*)x;
        Timed timed(kDraws);
        r_glDrawElements(c->mode, c->count, c->type, c->indices);
    };
    c->mode = mode;
    c->count = count;
    c->type = type;
    c->indices = indices;
    Publish(&c->cmd);
}

void Install() {
#define X(name)                 \
    if (glad_##name) {          \
        r_##name = glad_##name; \
        glad_##name = w_##name; \
    }
    GLT_EACH(X)
#undef X
}

void Remove() {
#define X(name)                 \
    if (r_##name) {             \
        glad_##name = r_##name; \
        r_##name = nullptr;     \
    }
    GLT_EACH(X)
#undef X
}

// ===[ The drawing thread ]===

struct Handover {
    bool ok;
    EGLint error;
};

void NoteBusy() {
    LONGLONG now = Ticks();
    gBusyTicks.fetch_add(now - gBusyFrom, std::memory_order_relaxed);
    gBusyFrom = now;
}

unsigned __stdcall Drawing(void* param) {
    Handover* handover = (Handover*)param;
    handover->ok = eglMakeCurrent(gDisplay, gDraw, gRead, gContext) != EGL_FALSE;
    if (!handover->ok) handover->error = eglGetError();
    bool ok = handover->ok;
    SetEvent(gAnswer);  // handover is the caller's again
    if (!ok) return 0;

    // REVC_GL_PRIORITY=0: as any other thread. Above them otherwise: a frame waits for this one
    // (fewer frames of over 50 ms that way, if not more frames). Keeping this thread and the
    // caller's on a core each was tried too (SetThreadAffinityMask, which kernel32 has on a
    // phone): the scheduler does move this one between three of the four, and it made no
    // difference.
    const char* priority = getenv("REVC_GL_PRIORITY");
    if (!priority || atoi(priority) != 0) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    uint32_t tail = gTail.load(std::memory_order_relaxed);
    gBusyFrom = Ticks();
    while (!gQuit) {
        uint32_t head = gHead.load(std::memory_order_acquire);
        if (head == tail) {
            NoteBusy();
            gSleeps.fetch_add(1, std::memory_order_relaxed);
            gAsleep.store(true, std::memory_order_seq_cst);
            if (gHead.load(std::memory_order_seq_cst) == tail) WaitForSingleObjectEx(gWake, INFINITE, FALSE);
            gAsleep.store(false, std::memory_order_seq_cst);
            gBusyFrom = Ticks();
            continue;
        }
        uint32_t ran = 0;
        while (tail != head) {
            Cmd* c = (Cmd*)(gRing + (tail & (kRing - 1)));
            uint32_t size = c->size;  // the command may be written over once the tail has moved
            c->run(c);
            tail += size;
            if (++ran % 128 == 0) gTail.store(tail, std::memory_order_release);
        }
        gTail.store(tail, std::memory_order_release);
        gCalls.fetch_add(ran, std::memory_order_relaxed);
    }
    return 0;
}

struct Posted {
    Cmd cmd;
    void (*fn)(void*);
    void* arg;
};

}  // namespace

bool glt_on(void) { return gOn; }

bool glt_start(void (*log)(const char* text)) {
    if (gOn) return true;
    gLog = log;
    gDisplay = eglGetCurrentDisplay();
    gContext = eglGetCurrentContext();
    gDraw = eglGetCurrentSurface(EGL_DRAW);
    gRead = eglGetCurrentSurface(EGL_READ);
    if (!glad_glGetIntegerv || gDisplay == EGL_NO_DISPLAY || gContext == EGL_NO_CONTEXT) {
        Log("glthread: no OpenGL ES context is current here, or glad has not looked its functions up");
        return false;
    }
    if (!gRing) gRing = (uint8_t*)_aligned_malloc(kRing, 64);
    if (!gWake) gWake = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!gAnswer) gAnswer = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!gShown) gShown = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!gRing || !gWake || !gAnswer || !gShown) {
        Log("glthread: no memory or no events for the queue");
        return false;
    }
    // what the functions that copy pixels have to know of the context's state
    gUnpackAlignment = 4;
    glad_glGetIntegerv(GL_UNPACK_ALIGNMENT, &gUnpackAlignment);
    gUnpackBuffer = 0;
    gUnpackOdd = false;
    // and of what the default vertex array has: bound, enabled, pointing into memory
    GLint value = 0, bound = 0;
    glad_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &value);
    gArrayBuffer = (GLuint)value;
    if (glad_glBindVertexArray) {
        glad_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &bound);
        if (bound) glad_glBindVertexArray(0);
    }
    gVertexArray = (GLuint)bound;
    value = 0;
    glad_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &value);
    gElementBuffer = (GLuint)value;
    gEnabledMask = gFromMemoryMask = 0;
    GLint attributes = 0;
    glad_glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
    for (GLint i = 0; i < attributes && i < 16 && glad_glGetVertexAttribiv && glad_glGetVertexAttribPointerv; i++) {
        GLint enabled = 0, buffer = 0, size = 4, type = GL_FLOAT, normalized = 0, stride = 0;
        void* pointer = nullptr;
        glad_glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        glad_glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        glad_glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glad_glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glad_glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
        glad_glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glad_glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
        if (enabled) gEnabledMask |= 1u << i;
        if (!buffer && (uintptr_t)pointer >= 65536) {
            gFromMemory[i] = {size, (GLenum)type, (GLboolean)normalized, stride, (const uint8_t*)pointer};
            gFromMemoryMask |= 1u << i;
        }
    }
    if (bound) glad_glBindVertexArray((GLuint)bound);
    while (glad_glGetError && glad_glGetError() != GL_NO_ERROR) {
    }

    gCaller = GetCurrentThreadId();
    gSeenTail = gTail.load();
    gFramesSent = 0;
    gFramesShown = 0;
    gQuit = false;
    gBuffers.left = gTextures.left = gVertexArrays.left = 0;

    if (!eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        Log("glthread: the context would not be let go of");
        return false;
    }
    // ANGLE compiles shaders where they are linked, HLSL and all: not on a small stack
    static Handover handover;
    handover.ok = false;
    handover.error = 0;
    gThread = (HANDLE)_beginthreadex(nullptr, 4u << 20, Drawing, &handover, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (gThread) WaitForSingleObjectEx(gAnswer, INFINITE, FALSE);
    if (!gThread || !handover.ok) {
        char text[120];
        snprintf(text, sizeof(text), "glthread: the drawing thread did not get the context (%s, EGL error 0x%04x)",
                 gThread ? "eglMakeCurrent" : "no thread", (unsigned)handover.error);
        Log(text);
        if (gThread) {
            WaitForSingleObjectEx(gThread, INFINITE, FALSE);
            CloseHandle(gThread);
            gThread = nullptr;
        }
        eglMakeCurrent(gDisplay, gDraw, gRead, gContext);
        return false;
    }
    Install();
    gOn = true;
    return true;
}

void glt_stop(void) {
    if (!gOn) return;
    Wait([] {
        // the names made ahead and never handed out
        if (gBuffers.left) r_glDeleteBuffers(gBuffers.left, gBuffers.names);
        if (gTextures.left) r_glDeleteTextures(gTextures.left, gTextures.names);
        if (gVertexArrays.left) r_glDeleteVertexArrays(gVertexArrays.left, gVertexArrays.names);
        eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        gQuit = true;
    });
    WaitForSingleObjectEx(gThread, INFINITE, FALSE);
    CloseHandle(gThread);
    gThread = nullptr;
    Remove();
    gOn = false;
    if (!eglMakeCurrent(gDisplay, gDraw, gRead, gContext)) Log("glthread: the context did not come back from the drawing thread");
}

void glt_post(void (*fn)(void*), void* arg) {
    if (!gOn) {
        fn(arg);
        return;
    }
    Posted* c = (Posted*)Reserve(Round(sizeof(Posted)));
    c->cmd.run = [](Cmd* x) {
        Posted* c = (Posted*)x;
        c->fn(c->arg);
    };
    c->fn = fn;
    c->arg = arg;
    Publish(&c->cmd);
}

void glt_call(void (*fn)(void*), void* arg) {
    if (!gOn) {
        fn(arg);
        return;
    }
    Wait([&] { fn(arg); });
}

void glt_frame(void (*show)(void*), void* arg) {
    if (!gOn) {
        show(arg);
        return;
    }
    Posted* c = (Posted*)Reserve(Round(sizeof(Posted)));
    c->cmd.run = [](Cmd* x) {
        Posted* c = (Posted*)x;
        c->fn(c->arg);
        NoteBusy();
        gFramesShown.fetch_add(1, std::memory_order_release);
        SetEvent(gShown);
    };
    c->fn = show;
    c->arg = arg;
    Publish(&c->cmd);
    gFramesSent++;
    // this frame may wait in the queue while the one before it is drawn, and no more than that:
    // what the player does is a frame old on the screen, not two
    LONGLONG from = Ticks();
    while (gFramesSent - gFramesShown.load(std::memory_order_acquire) > 1) WaitForSingleObjectEx(gShown, INFINITE, FALSE);
    gFrameTicks += Ticks() - from;
}

void glt_stats(GltStats* stats) {
    stats->drawingMs = Milliseconds(gBusyTicks.exchange(0, std::memory_order_relaxed));
    stats->frameWaitMs = Milliseconds(gFrameTicks);
    stats->answerWaitMs = Milliseconds(gAnswerTicks);
    stats->calls = gCalls.exchange(0, std::memory_order_relaxed);
    stats->sleeps = gSleeps.exchange(0, std::memory_order_relaxed);
    stats->answers = gAnswers;
    stats->timesFull = gTimesFull;
    gFrameTicks = gAnswerTicks = 0;
    gAnswers = gTimesFull = 0;
    for (int kind = 0; kind < kKinds; kind++) {
        stats->kindMs[kind] = Milliseconds(gKindTicks[kind].exchange(0, std::memory_order_relaxed));
        stats->kindCalls[kind] = gKindCalls[kind].exchange(0, std::memory_order_relaxed);
    }
    for (int kind = 0; kind < 2; kind++) {
        stats->longDrawMs[kind] = Milliseconds(gLongDrawTicks[kind].exchange(0, std::memory_order_relaxed));
        stats->longDraws[kind] = gLongDraws[kind].exchange(0, std::memory_order_relaxed);
    }
    stats->memoryDrawKB = gMemoryDrawBytes.exchange(0, std::memory_order_relaxed) / 1024.0;
}

void glt_timing(bool on) { gTiming.store(on, std::memory_order_relaxed); }
