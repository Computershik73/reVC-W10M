// librw on ANGLE through the port's GLFW layer (w10m/glfw), on the PC that builds the port.
//
// There is no shell here and no screen: the "panel" is absent, so the layer draws into a
// pbuffer. What is checked is what a phone decides before the first frame: that EGL gives a
// context (OpenGL ES 3.0, or 2.0 when REVC_ANGLE_FL caps Direct3D at 9_3 or 9_1, which is
// what most Windows phones are), that librw starts on it, and that every shader program
// librw and the game build compiles and links there: Direct3D's level 9 shader models are
// much smaller than what desktop OpenGL takes. Then one clear, read back.
//
// A desktop GPU held to 9_3 follows that level's rules; it is not a phone's driver.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <rw.h>

#include "glthread.h"
#include "host_api.h"

using namespace rw;
using namespace rw::gl3;

// ===[ The shell's side, for nobody ]===

extern "C" void* host_panel(void) { return nullptr; }
extern "C" bool host_panel_size(int32_t* w, int32_t* h) {
    *w = 1280;
    *h = 720;
    return true;
}
extern "C" bool host_panel_native_size(int32_t* w, int32_t* h) { return host_panel_size(w, h); }
extern "C" bool host_poll_event(HostEvent*) { return false; }
extern "C" bool host_has_focus(void) { return true; }
extern "C" void host_pad_state(HostPad* pad) {
    memset(pad, 0, sizeof(*pad));
    pad->axes[HOST_AXIS_LT] = pad->axes[HOST_AXIS_RT] = -1.0f;
}
extern "C" void host_set_context(int32_t) {}
extern "C" int32_t host_take_requests(void) { return 0; }
extern "C" const wchar_t* host_local_folder(void) { return nullptr; }
extern "C" void host_set_pad_style(int32_t) {}
extern "C" void host_log(const char* text) { std::printf("      %s\n", text); }
extern "C" void host_logf(const char* format, ...) {
    char text[1024];
    va_list va;
    va_start(va, format);
    vsnprintf(text, sizeof(text), format, va);
    va_end(va);
    host_log(text);
}
extern "C" void host_fatal(const char* text) {
    std::printf("FAIL  fatal: %s\n", text);
    exit(2);
}
extern "C" int32_t reVC_W10M_Frame(void) { return HOST_CONTEXT_LOADING; }
extern "C" void reVC_W10M_SkipCutscenes(void) {}

// ===[ The checks ]===

static int failures;

static void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

#define FULL_LIGHT "#define DIRECTIONALS\n#define POINTLIGHTS\n#define SPOTLIGHTS\n"
#define NO_ALPHA_TEST "#define NO_ALPHATEST\n"

static void Program(const char* name, const char* vertexDefines, const char* vertex, const char* fragmentDefines,
                    const char* fragment) {
    const char* vs[] = {shaderDecl, vertexDefines, header_vert_src, vertex, nil};
    const char* fs[] = {shaderDecl, fragmentDefines, header_frag_src, fragment, nil};
    Shader* shader = Shader::create(vs, fs);
    Check(shader != nil, name);
    if (shader) shader->destroy();
}

// The same sources and the same defines as src/gl/gl3device.cpp, gl3immed.cpp, gl3matfx.cpp and
// gl3skin.cpp of librw put together
static void LibrwPrograms(void) {
#include "shaders/default_vs_gl.inc"
#include "shaders/im2d_gl.inc"
#include "shaders/im3d_gl.inc"
#include "shaders/matfx_gl.inc"
#include "shaders/simple_fs_gl.inc"
#include "shaders/skin_gl.inc"
    Program("librw: default", "", default_vert_src, "", simple_frag_src);
    Program("librw: default, no alpha test", "", default_vert_src, NO_ALPHA_TEST, simple_frag_src);
    Program("librw: default, all lights", FULL_LIGHT, default_vert_src, "", simple_frag_src);
    Program("librw: default, all lights, no alpha test", FULL_LIGHT, default_vert_src, NO_ALPHA_TEST, simple_frag_src);
    Program("librw: im2d", "", im2d_vert_src, "", simple_frag_src);
    Program("librw: im3d", "", im3d_vert_src, "", simple_frag_src);
    Program("librw: environment map", "", matfx_env_vert_src, "", matfx_env_frag_src);
    Program("librw: environment map, no alpha test", "", matfx_env_vert_src, NO_ALPHA_TEST, matfx_env_frag_src);
    Program("librw: environment map, all lights", FULL_LIGHT, matfx_env_vert_src, "", matfx_env_frag_src);
    Program("librw: environment map, all lights, no alpha test", FULL_LIGHT, matfx_env_vert_src, NO_ALPHA_TEST,
            matfx_env_frag_src);
    Program("librw: skin", "", skin_vert_src, "", simple_frag_src);
    Program("librw: skin, no alpha test", "", skin_vert_src, NO_ALPHA_TEST, simple_frag_src);
    Program("librw: skin, all lights", FULL_LIGHT, skin_vert_src, "", simple_frag_src);
    Program("librw: skin, all lights, no alpha test", FULL_LIGHT, skin_vert_src, NO_ALPHA_TEST, simple_frag_src);
}

// As src/extras/custompipes_gl.cpp, postfx.cpp and screendroplets.cpp of the game do
static void GamePrograms(void) {
#include "shaders/obj/colourfilterVC_frag.inc"
#include "shaders/obj/contrast_frag.inc"
#include "shaders/obj/default_UV2_vert.inc"
#include "shaders/obj/im2d_UV2_vert.inc"
#include "shaders/obj/im2d_vert.inc"
#include "shaders/obj/neoGloss_frag.inc"
#include "shaders/obj/neoGloss_vert.inc"
#include "shaders/obj/neoRimSkin_vert.inc"
#include "shaders/obj/neoRim_vert.inc"
#include "shaders/obj/neoVehicle_frag.inc"
#include "shaders/obj/neoVehicle_vert.inc"
#include "shaders/obj/neoWorldVC_frag.inc"
#include "shaders/obj/screenDroplet_frag.inc"
#include "shaders/obj/simple_frag.inc"
    Program("game: vehicles (neo)", "#define DIRECTIONALS\n", neoVehicle_vert_src, "", neoVehicle_frag_src);
    Program("game: world light maps (neo)", "", default_UV2_vert_src, "", neoWorldVC_frag_src);
    Program("game: gloss (neo)", "", neoGloss_vert_src, "", neoGloss_frag_src);
    Program("game: rim light, skinned (neo)", "#define DIRECTIONALS\n", neoRimSkin_vert_src, "", simple_frag_src);
    Program("game: rim light (neo)", "#define DIRECTIONALS\n", neoRim_vert_src, "", simple_frag_src);
    Program("game: colour filter", "", im2d_vert_src, "", colourfilterVC_frag_src);
    Program("game: contrast", "", im2d_vert_src, "", contrast_frag_src);
    Program("game: screen droplets", "", im2d_UV2_vert_src, "", screenDroplet_frag_src);
}

// One triangle through librw's own skin shader, as it is built for this context: with whole
// bone matrices, or with their rows where a vertex shader has too few uniforms for 64 matrices
// (Direct3D level 9). The triangle is drawn on the left; its bone moves it one unit to the
// right. Where it lands says whether the shader and the matrices it is given agree.
static void SkinnedTriangle(int width, int height) {
#include "shaders/simple_fs_gl.inc"
#include "shaders/skin_gl.inc"
    const char* vs[] = {shaderDecl, header_vert_src, skin_vert_src, nil};
    const char* fs[] = {shaderDecl, NO_ALPHA_TEST, header_frag_src, simple_frag_src, nil};
    Shader* shader = Shader::create(vs, fs);
    if (!shader) {
        Check(false, "the skin shader, to draw with");
        return;
    }
    GLuint program = shader->program;
    glUseProgram(program);

    // 64 bones in librw's Matrix layout: right, up, at, pos, each with a fourth float that
    // is flags or padding. Not zero here: the shader must not use it.
    static float matrices[64 * 16], rows[64 * 12];
    for (int bone = 0; bone < 64; bone++) {
        float* m = &matrices[bone * 16];
        for (int i = 0; i < 16; i++) m[i] = 0.0f;
        m[0] = m[5] = m[10] = 1.0f;
        m[3] = m[7] = m[11] = m[15] = 123.0f;
    }
    matrices[1 * 16 + 12] = 1.0f;  // bone 1: one unit along x
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (bonesAsRows) {
        packBoneRows(rows, matrices, 64);
        glUniform4fv(glGetUniformLocation(program, "u_boneRows"), 64 * 3, rows);
    } else {
        glUniformMatrix4fv(glGetUniformLocation(program, "u_boneMatrices"), 64, GL_FALSE, matrices);
    }
    glUniformMatrix4fv(glGetUniformLocation(program, "u_proj"), 1, GL_FALSE, identity);
    glUniformMatrix4fv(glGetUniformLocation(program, "u_view"), 1, GL_FALSE, identity);
    glUniformMatrix4fv(glGetUniformLocation(program, "u_world"), 1, GL_FALSE, identity);
    glUniform4f(glGetUniformLocation(program, "u_ambLight"), 1, 1, 1, 1);
    glUniform4f(glGetUniformLocation(program, "u_surfProps"), 1, 0, 0, 0);  // all ambient
    glUniform4f(glGetUniformLocation(program, "u_matColor"), 0, 1, 0, 1);   // green
    glUniform4f(glGetUniformLocation(program, "u_fogData"), 0, 0, 0, 1);    // no fog
    glUniform1i(glGetUniformLocation(program, "tex0"), 0);

    const unsigned char white[4] = {255, 255, 255, 255};
    GLuint texture = 0, buffer = 0;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);

    // position (3), bone weights (4), bone indices (4); around x = -0.5, on bone 1
    const float vertices[] = {
        -0.7f, -0.3f, 0.0f, 1, 0, 0, 0, 1, 0, 0, 0,
        -0.3f, -0.3f, 0.0f, 1, 0, 0, 0, 1, 0, 0, 0,
        -0.5f, 0.3f,  0.0f, 1, 0, 0, 0, 1, 0, 0, 0,
    };
    const GLsizei stride = 11 * sizeof(float);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(ATTRIB_POS);
    glVertexAttribPointer(ATTRIB_POS, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(ATTRIB_WEIGHTS);
    glVertexAttribPointer(ATTRIB_WEIGHTS, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(ATTRIB_INDICES);
    glVertexAttribPointer(ATTRIB_INDICES, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(7 * sizeof(float)));
    glVertexAttrib3f(ATTRIB_NORMAL, 0, 0, 1);
    glVertexAttrib4f(ATTRIB_COLOR, 0, 0, 0, 1);
    glVertexAttrib2f(ATTRIB_TEXCOORDS0, 0, 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    unsigned char left[4] = {}, right[4] = {};
    glReadPixels(width / 4, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
    glReadPixels(width * 3 / 4, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
    std::printf("      skin by %s: where it was drawn %d,%d,%d; one unit to the right %d,%d,%d\n",
                bonesAsRows ? "rows (u_boneRows)" : "matrices (u_boneMatrices)", left[0], left[1], left[2], right[0],
                right[1], right[2]);
    Check(left[0] == 0 && left[1] == 0 && left[2] == 0, "the bone moved the triangle away from where it was drawn");
    Check(right[0] == 0 && right[1] == 255 && right[2] == 0, "and to where the bone's matrix puts it, in the material's colour");
    Check(glGetError() == GL_NO_ERROR, "no OpenGL error while drawing it");

    glDisableVertexAttribArray(ATTRIB_POS);
    glDisableVertexAttribArray(ATTRIB_WEIGHTS);
    glDisableVertexAttribArray(ATTRIB_INDICES);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &buffer);
    glDeleteTextures(1, &texture);
    glUseProgram(0);
    shader->destroy();
}

// What librw itself draws the game with: geometry instanced once and drawn again and again.
// Where there are vertex array objects (at every level here) an atomic's vertex input is
// recorded when it is instanced and only bound afterwards, and what is drawn in between in
// immediate mode, on the default vertex array, must leave it as it was. Two atomics and an
// immediate triangle, each over the whole picture in its own colour, one after another.
static Atomic* WholePicture(uint8 red, uint8 green, uint8 blue) {
    Geometry* geometry = Geometry::create(3, 1, Geometry::POSITIONS | Geometry::PRELIT | Geometry::MODULATE);
    const V3d corners[3] = {{-3.0f, -3.0f, 10.0f}, {3.0f, -3.0f, 10.0f}, {0.0f, 3.0f, 10.0f}};
    for (int i = 0; i < 3; i++) {
        geometry->morphTargets[0].vertices[i] = corners[i];
        geometry->colors[i] = {255, 255, 255, 255};
        geometry->triangles[0].v[i] = static_cast<uint16>(i);
    }
    Material* material = Material::create();
    material->color = {red, green, blue, 255};
    geometry->triangles[0].matId = static_cast<uint16>(geometry->matList.appendMaterial(material));
    geometry->calculateBoundingSphere();
    geometry->buildMeshes();
    Atomic* atomic = Atomic::create();
    atomic->setFrame(Frame::create());
    atomic->setGeometry(geometry, 0);
    return atomic;
}

static bool CentreIs(int width, int height, int red, int green, int blue, const char* what) {
    unsigned char pixel[4] = {};
    glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    bool ok = pixel[0] == red && pixel[1] == green && pixel[2] == blue;
    if (!ok) std::printf("      %s: %d,%d,%d where %d,%d,%d was drawn\n", what, pixel[0], pixel[1], pixel[2], red, green, blue);
    return ok;
}

static void InstancedAtomics(int width, int height) {
    Camera* camera = Camera::create();
    camera->setFrame(Frame::create());
    camera->frameBuffer = Raster::create(width, height, 0, Raster::CAMERA);
    camera->zBuffer = Raster::create(width, height, 0, Raster::ZBUFFER);
    camera->setNearPlane(0.1f);
    camera->setFarPlane(100.0f);
    V2d window = {1.0f, 1.0f};
    camera->setViewWindow(&window);
    camera->setProjection(Camera::PARALLEL);

    Atomic* red = WholePicture(255, 0, 0);
    Atomic* blue = WholePicture(0, 0, 255);
    camera->beginUpdate();
    SetRenderState(ZTESTENABLE, 0);
    SetRenderState(ZWRITEENABLE, 0);
    SetRenderState(CULLMODE, CULLNONE);
    SetRenderState(FOGENABLE, 0);
    SetRenderStatePtr(TEXTURERASTER, nil);

    red->render();  // instanced here
    bool ok = CentreIs(width, height, 255, 0, 0, "the first atomic");
    blue->render();
    ok = CentreIs(width, height, 0, 0, 255, "the second atomic") && ok;
    red->render();  // from what was recorded
    ok = CentreIs(width, height, 255, 0, 0, "the first atomic again") && ok;

    Im2DVertex corners[3];
    const float at[3][2] = {{-(float)width, -(float)height}, {3.0f * width, -(float)height}, {width / 2.0f, 3.0f * height}};
    for (int i = 0; i < 3; i++) {
        corners[i].setScreenX(at[i][0]);
        corners[i].setScreenY(at[i][1]);
        corners[i].setScreenZ(0.5f);
        corners[i].setRecipCameraZ(1.0f);
        corners[i].setColor(0, 255, 0, 255);
        corners[i].setU(0.0f, 1.0f);
        corners[i].setV(0.0f, 1.0f);
    }
    im2d::RenderPrimitive(PRIMTYPETRILIST, corners, 3);
    ok = CentreIs(width, height, 0, 255, 0, "the immediate triangle") && ok;

    blue->render();
    ok = CentreIs(width, height, 0, 0, 255, "the second atomic after it") && ok;
    red->render();
    ok = CentreIs(width, height, 255, 0, 0, "the first atomic after it") && ok;

    // Shared buffers (OpenGL ES 2.0 with vertex array objects): the two geometries lie side
    // by side in one page, the second drawn with indices that count from the page's first
    // vertex. When the first is gone a third takes its place; instanced a second time, that
    // one leaves the page for buffers of its own. The neighbour is drawn right each time.
    if (gl3Caps.sharedBuffers) {
        gl3::InstanceDataHeader* first = (gl3::InstanceDataHeader*)red->geometry->instData;
        gl3::InstanceDataHeader* second = (gl3::InstanceDataHeader*)blue->geometry->instData;
        bool together = first->page && first->page == second->page && first->vao == second->vao &&
                        second->firstVertex == first->firstVertex + first->totalNumVertex;
        Geometry* gone = red->geometry;
        red->destroy();
        gone->destroy();  // WholePicture()'s own reference: the geometry and its place in the page go
        Atomic* green = WholePicture(0, 255, 0);
        green->render();
        ok = CentreIs(width, height, 0, 255, 0, "a third atomic, where the first was") && ok;
        gl3::InstanceDataHeader* third = (gl3::InstanceDataHeader*)green->geometry->instData;
        bool reused = third->page == second->page && third->firstVertex == 0;
        blue->render();
        ok = CentreIs(width, height, 0, 0, 255, "the second atomic, its neighbour replaced") && ok;
        green->geometry->lock(Geometry::LOCKVERTICES);
        green->geometry->unlock();
        green->render();
        ok = CentreIs(width, height, 0, 255, 0, "the third atomic, instanced again") && ok;
        third = (gl3::InstanceDataHeader*)green->geometry->instData;
        bool left = third->page == nil && third->vbo != second->vbo;
        blue->render();
        ok = CentreIs(width, height, 0, 0, 255, "the second atomic, alone in the page") && ok;
        std::printf("      shared buffers: two geometries in one page %s, room given back and taken again %s, "
                    "a geometry instanced again in buffers of its own %s\n",
                    together ? "yes" : "NO", reused ? "yes" : "NO", left ? "yes" : "NO");
        ok = together && reused && left && ok;
    } else {
        std::printf("      shared buffers: none\n");
    }
    camera->endUpdate();
    std::printf("      vertex array objects: %s\n", gl3Caps.vertexArrays ? "in use" : "none");
    Check(ok, "atomics drawn again from their recorded vertex input, an immediate triangle between them");
    Check(glGetError() == GL_NO_ERROR, "no OpenGL error while drawing them");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // in step with what librw prints on stderr, and kept if it crashes
    static GLFWwindow* window;
    static EngineOpenParams parameters;
    parameters.window = &window;
    parameters.width = 1280;
    parameters.height = 720;
    parameters.windowtitle = "reVC";

    Check(Engine::init(), "librw initialises");
    // the plugins the game attaches (PluginAttach in src/core/main.cpp): two of them make shaders
    registerMeshPlugin();
    registerNativeDataPlugin();
    registerAtomicRightsPlugin();
    registerMaterialRightsPlugin();
    xbox::registerVertexFormatPlugin();
    registerSkinPlugin();
    registerHAnimPlugin();
    registerMatFXPlugin();
    Check(Engine::open(&parameters), "the device opens (GLFW's API over EGL)");

    VideoMode mode;
    int modes = Engine::getNumVideoModes();
    for (int i = 0; i < modes; i++)
        if (Engine::getVideoModeInfo(&mode, i))
            std::printf("      mode %d: %dx%dx%d%s\n", i, mode.width, mode.height, mode.depth,
                        mode.flags & VIDEOMODEEXCLUSIVE ? ", exclusive" : "");
    Check(modes >= 2, "a windowed and an exclusive video mode, as the game's device selection expects");

    // REVC_TEST_PROGRAM_CACHE: a folder for librw's kept programs (tools/test_render.py runs
    // every level twice with one: the second run must find them all there)
    if (const char* cache = getenv("REVC_TEST_PROGRAM_CACHE")) programCacheDir = cache;
    if (!Engine::start()) {
        Check(false, "librw starts: a context, its own shaders");
        std::printf("FAILED\n");
        return 1;
    }
    Check(true, "librw starts: a context, its own shaders");
    Check(window != nil, "the window is handed back to the game");

    // REVC_TEST_GL_THREAD: everything from here on with OpenGL called on the drawing thread
    // (w10m/glthread), as on a phone with a GPU of Direct3D level 9 after its first frame
    if (getenv("REVC_TEST_GL_THREAD"))
        Check(glt_start(host_log) && glt_on(), "OpenGL ES moves to a thread of its own");

    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    std::printf("      %s\n      %s\n", version ? version : "?", renderer ? renderer : "?");
    std::printf("      librw: OpenGL ES %d.%d, DXT textures %s, anisotropy up to %.0f\n", gl3Caps.glversion / 10,
                gl3Caps.glversion % 10, gl3Caps.dxtSupported ? "yes" : "NO", gl3Caps.maxAnisotropy);
    Check(gl3Caps.gles != 0, "the context is OpenGL ES");
    Check(gl3Caps.dxtSupported != 0, "DXT textures (the game's are)");
    Check(defaultShader && defaultShader_noAT && defaultShader_fullLight && defaultShader_fullLight_noAT,
          "librw's default shaders exist after the start");

    // first, while the context is as librw left it: the checks after this one use OpenGL
    // behind its back (programs, textures, buffers of their own)
    {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        InstancedAtomics(width, height);
    }

    LibrwPrograms();
    GamePrograms();

    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    Check(width == 1280 && height == 720, "the render target has the panel's size");
    glViewport(0, 0, width, height);
    glClearColor(1.0f, 0.0f, 0.5f, 1.0f);
    glClearDepthf(1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    unsigned char pixel[4] = {};
    glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    std::printf("      cleared to 255,0,128 and read back %d,%d,%d\n", pixel[0], pixel[1], pixel[2]);
    Check(pixel[0] == 255 && pixel[1] == 0 && pixel[2] >= 127 && pixel[2] <= 128, "a clear lands in the surface");
    GLint depthBits = 0, stencilBits = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depthBits);
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
    Check(depthBits >= 16 && stencilBits == 8, "the surface has depth and stencil");
    Check(glGetError() == GL_NO_ERROR, "no OpenGL error on the way");

    SkinnedTriangle(width, height);

    // the programs: all compiled, or all found where the last run kept them
    std::printf("      programs: %d compiled, %d from the folder of the last run\n", (int)numProgramsCompiled,
                (int)numProgramsLoaded);
    if (getenv("REVC_TEST_PROGRAMS_KEPT"))
        Check(gl3Caps.programBinaries && numProgramsCompiled == 0 && numProgramsLoaded > 0,
              "every program came from the folder the run before left them in");

    glfwPollEvents();
    glfwSwapInterval(1);
    glfwSwapBuffers(window);
    // a few frames more: after its first an OpenGL ES 2.0 context is the drawing thread's
    // whatever the setting, and a frame's end there is not a swap alone
    for (int frame = 0; frame < 4; frame++) {
        glClearColor(0.0f, frame * 0.25f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (frame == 3) {
            glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            Check(pixel[0] == 0 && pixel[1] >= 190 && pixel[1] <= 192 && pixel[2] == 255,
                  "a later frame's clear is read back as it was asked for");
        }
        glfwSwapBuffers(window);
    }
    Check(glGetError() == GL_NO_ERROR, "no OpenGL error in those frames");
    if (glt_on()) {
        GltStats stats;
        glt_stats(&stats);
        std::printf("      the drawing thread: %u calls made, %u of them waited for\n", (unsigned)stats.calls,
                    (unsigned)stats.answers);
        Check(stats.calls > 0, "the drawing thread made the calls");
    }

    Engine::stop();
    Engine::close();
    Engine::term();
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
