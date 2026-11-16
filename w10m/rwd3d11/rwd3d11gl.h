// The Direct3D 11 renderer of the Windows 10 Mobile port (w10m/rwd3d11) has the interface of
// librw's OpenGL renderer (namespace rw::gl3: that is what the game's own pipelines and effects
// are written against) and none of its OpenGL. librw's rwgl3.h includes this header in the
// place of OpenGL's (RW_D3D11): the few OpenGL names that code written against the interface
// uses as plain values, and the two calls it makes for uniforms of its own.
#ifndef RWD3D11GL_H
#define RWD3D11GL_H

typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef float GLfloat;
typedef int GLsizei;
typedef unsigned char GLboolean;

#ifndef GL_FLOAT
#define GL_FALSE 0
#define GL_TRUE 1
// what a vertex attribute is made of (rw::gl3::AttribDesc)
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
// how the indices of a geometry are to be read (rw::gl3::InstanceDataHeader::primType)
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
// what a texture is made of (rw::gl3::Gl3Raster::internalFormat)
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_RGB5_A1 0x8057
#define GL_DEPTH_STENCIL 0x84F9
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

namespace rw {
namespace gl3 {
// A uniform the application manages itself (registered without a type), for the shader in use:
// what glUniform does. `id` is what registerUniform gave, or -1.
void setUniformRaw(int id, const float *data, int numFloats);
}
}

// currentShader->uniformLocations[id] is the id itself where the shader has the uniform
inline void glUniform4fv(int location, int count, const float *v) { rw::gl3::setUniformRaw(location, v, count*4); }
inline void glUniform3fv(int location, int count, const float *v) { rw::gl3::setUniformRaw(location, v, count*3); }

#endif
