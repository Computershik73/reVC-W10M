// ===[ What a draw costs here (REVC_BENCH=1) ]===
// The same small quad drawn a few hundred times, with one thing changed between the draws at
// a time: what this phone's driver, and ANGLE in front of it, charge for a draw as such and
// for each kind of change. Once, when the menu first comes up; the lines go to the log.
// The same benchmark through Direct3D 11 itself is d3dbench.cpp; the game's skeleton
// (src/skel/w10m/w10m.cpp) runs both when the menu first comes up, the render test on the
// PC (test/render) after its checks.
#include <cstring>

#include <rw.h>

#include "host_api.h"

static double
BenchNow(void)
{
	return (double)rw::gl3::perfNow() * 1e-3;	// microseconds
}

static GLuint
BenchShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nil);
	glCompileShader(shader);
	return shader;
}

extern "C" void
W10M_DrawBenchmark(void)
{
	enum { N = 400 };
	static const char *vsrc =
		"attribute vec3 in_pos;\nattribute vec2 in_tex;\nuniform mat4 u_m;\nuniform vec4 u_big[64];\nvarying vec2 v_tex;\n"
		"void main(){ v_tex = in_tex; gl_Position = u_m*vec4(in_pos.xy, 0.0, 1.0) + u_big[int(in_pos.z)]*0.0; }\n";
	static const char *fsrc =
		"precision mediump float;\nuniform sampler2D tex;\nuniform vec4 u_col;\nvarying vec2 v_tex;\n"
		"void main(){ gl_FragColor = texture2D(tex, v_tex)*u_col; }\n";
	// a quad of a few pixels in a corner: the time is to be the calls', not the pixels'
	static const float verts[4*5] = {
		-0.99f, -0.99f, 0.0f, 0.0f, 0.0f,   -0.98f, -0.99f, 1.0f, 1.0f, 0.0f,
		-0.98f, -0.98f, 2.0f, 1.0f, 1.0f,   -0.99f, -0.98f, 3.0f, 0.0f, 1.0f,
	};
	static const unsigned short indices[6] = { 0, 1, 2, 0, 2, 3 };
	static const float identity[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
	static float big[64*4];
	static unsigned char pixels[4*4*4];
	static unsigned char scratch[360000];

	GLint oldProgram = 0, oldTexture = 0, oldArray = 0, oldActive = 0, oldDepthMask = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArray);
	glGetIntegerv(GL_DEPTH_WRITEMASK, &oldDepthMask);
	GLboolean oldBlend = glIsEnabled(GL_BLEND), oldDepth = glIsEnabled(GL_DEPTH_TEST), oldCull = glIsEnabled(GL_CULL_FACE);
	rw::gl3::useDefaultVertexArray();
	GLint oldElements = 0, oldCullFace = GL_BACK, oldDepthFunc = GL_LEQUAL, oldBlendSrc = GL_ONE, oldBlendDst = GL_ZERO;
	glGetIntegerv(GL_CULL_FACE_MODE, &oldCullFace);
	glGetIntegerv(GL_DEPTH_FUNC, &oldDepthFunc);
	glGetIntegerv(GL_BLEND_SRC_RGB, &oldBlendSrc);
	glGetIntegerv(GL_BLEND_DST_RGB, &oldBlendDst);
	glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &oldElements);

	GLuint vs = BenchShader(GL_VERTEX_SHADER, vsrc), fs = BenchShader(GL_FRAGMENT_SHADER, fsrc);
	GLuint program = glCreateProgram();
	glBindAttribLocation(program, 0, "in_pos");
	glBindAttribLocation(program, 1, "in_tex");
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	GLint linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		host_log("bench: its program did not link");
		glDeleteProgram(program);
		return;
	}
	glUseProgram(program);
	GLint u_m = glGetUniformLocation(program, "u_m"), u_big = glGetUniformLocation(program, "u_big"),
	      u_col = glGetUniformLocation(program, "u_col");
	glUniformMatrix4fv(u_m, 1, GL_FALSE, identity);
	glUniform4fv(u_big, 64, big);
	glUniform4f(u_col, 1.0f, 1.0f, 1.0f, 1.0f);
	glUniform1i(glGetUniformLocation(program, "tex"), 0);

	// a second program of the same sources: what changing programs costs
	GLuint vs2 = BenchShader(GL_VERTEX_SHADER, vsrc), fs2 = BenchShader(GL_FRAGMENT_SHADER, fsrc);
	GLuint program2 = glCreateProgram();
	glBindAttribLocation(program2, 0, "in_pos");
	glBindAttribLocation(program2, 1, "in_tex");
	glAttachShader(program2, vs2);
	glAttachShader(program2, fs2);
	glLinkProgram(program2);
	glUseProgram(program2);
	GLint u_m2 = glGetUniformLocation(program2, "u_m"), u_col2 = glGetUniformLocation(program2, "u_col");
	glUniformMatrix4fv(u_m2, 1, GL_FALSE, identity);
	glUniform4fv(glGetUniformLocation(program2, "u_big"), 64, big);
	glUniform4f(u_col2, 1.0f, 1.0f, 1.0f, 1.0f);
	glUniform1i(glGetUniformLocation(program2, "tex"), 0);
	glUseProgram(program);

	GLuint buffers[4], textures[2], vaos[2] = { 0, 0 };
	glGenBuffers(4, buffers);
	glGenTextures(2, textures);
	memset(pixels, 0xFF, sizeof(pixels));
	for (int i = 0; i < 2; i++) {
		glBindBuffer(GL_ARRAY_BUFFER, buffers[i]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2 + i]);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
		glBindTexture(GL_TEXTURE_2D, textures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	}
	#define BENCH_INPUT(i) do { \
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2 + (i)]); \
		glBindBuffer(GL_ARRAY_BUFFER, buffers[i]); \
		glEnableVertexAttribArray(0); \
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void*)0); \
		glEnableVertexAttribArray(1); \
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void*)12); \
	} while (0)
	if (rw::gl3::gl3Caps.vertexArrays) {
		glGenVertexArrays(2, vaos);
		for (int i = 0; i < 2; i++) {
			glBindVertexArray(vaos[i]);
			BENCH_INPUT(i);
		}
		glBindVertexArray(rw::gl3::defaultVertexArray);
	}
	BENCH_INPUT(0);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDepthMask(GL_FALSE);

	// warm: the first draws compile what is left to compile
	for (int i = 0; i < 20; i++)
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nil);
	glFinish();

	static const char *names[] = {
		"nothing changed", "a vec4 for the fragment shader", "a mat4 for the vertex shader", "both",
		"64 vec4 for the vertex shader", "the other texture", "the same vertex input described again",
		"the other buffers, described", "the other vertex array object", "blending on or off", "depth writing on or off",
		"glDrawArrays, nothing changed", "new vertices, glBufferData of their size",
		"new vertices, a 360 KB buffer orphaned and filled in part",
		"the other program", "the other program and a mat4 for it", "culling on or off", "the other face culled",
		"depth test on: depth writing on or off", "depth test on: the other depth function", "the other blend function",
		"a texture's filter changed", "nothing changed, again",
		// ways to draw what is new with every draw (librw's immediate mode: the HUD, the effects)
		"new vertices and indices as librw hands them over (glBufferData each, the input described)",
		"new vertices and indices by glBufferSubData at the start of buffers made once",
		"new vertices and indices by glBufferSubData further along each time, the input as it was",
		"new vertices and indices from memory, no buffers",
		"all the vertices and indices handed over once, each draw its part",
	};
	static float manyVerts[N*20];
	static unsigned short manyIndices[N*6];
	for (int regime = 0; regime < 28; regime++) {
		if (regime == 8 && !rw::gl3::gl3Caps.vertexArrays)
			continue;
		if (regime == 24 || regime == 25) {
			BENCH_INPUT(1);
			glBufferData(GL_ARRAY_BUFFER, 65536, nil, GL_STREAM_DRAW);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, 65536, nil, GL_STREAM_DRAW);
		}
		glFinish();
		double t0 = BenchNow();
		if (regime == 27) {
			for (int i = 0; i < N; i++) {
				memcpy(&manyVerts[i*20], verts, sizeof(verts));
				for (int j = 0; j < 6; j++)
					manyIndices[i*6 + j] = (unsigned short)(i*4 + indices[j]);
			}
			BENCH_INPUT(1);
			glBufferData(GL_ARRAY_BUFFER, sizeof(manyVerts), manyVerts, GL_STATIC_DRAW);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(manyIndices), manyIndices, GL_STATIC_DRAW);
		}
		for (int i = 0; i < N; i++) {
			float f = (i & 1) ? 1.0f : 0.99f;
			unsigned short moved[6];
			switch (regime) {
			case 23:
				glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
				glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2]);
				glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STREAM_DRAW);
				BENCH_INPUT(0);
				break;
			case 24:
				glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
				glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(indices), indices);
				break;
			case 25:
				for (int j = 0; j < 6; j++)
					moved[j] = (unsigned short)(i*4 + indices[j]);
				glBufferSubData(GL_ARRAY_BUFFER, i*sizeof(verts), sizeof(verts), verts);
				glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, i*sizeof(indices), sizeof(indices), moved);
				break;
			case 26:
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
				glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, verts);
				glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (const char*)verts + 12);
				break;
			case 1: glUniform4f(u_col, f, 1.0f, 1.0f, 1.0f); break;
			case 2: glUniformMatrix4fv(u_m, 1, GL_FALSE, identity); break;
			case 3: glUniform4f(u_col, f, 1.0f, 1.0f, 1.0f); glUniformMatrix4fv(u_m, 1, GL_FALSE, identity); break;
			case 4: big[4] = f; glUniform4fv(u_big, 64, big); break;
			case 5: glBindTexture(GL_TEXTURE_2D, textures[i & 1]); break;
			case 6: BENCH_INPUT(0); break;
			case 7: BENCH_INPUT(i & 1); break;
			case 8: glBindVertexArray(vaos[i & 1]); break;
			case 9: if (i & 1) glEnable(GL_BLEND); else glDisable(GL_BLEND); break;
			case 10: glDepthMask((i & 1) ? GL_TRUE : GL_FALSE); break;
			case 12: glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW); break;
			case 13:
				glBufferData(GL_ARRAY_BUFFER, sizeof(scratch), nil, GL_STREAM_DRAW);
				glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
				break;
			case 14: glUseProgram((i & 1) ? program2 : program); break;
			case 15:
				glUseProgram((i & 1) ? program2 : program);
				glUniformMatrix4fv((i & 1) ? u_m2 : u_m, 1, GL_FALSE, identity);
				break;
			case 16: if (i & 1) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE); break;
			case 17: glEnable(GL_CULL_FACE); glCullFace((i & 1) ? GL_FRONT : GL_BACK); break;
			case 18: glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask((i & 1) ? GL_TRUE : GL_FALSE); break;
			case 19: glEnable(GL_DEPTH_TEST); glDepthFunc((i & 1) ? GL_ALWAYS : GL_LEQUAL); break;
			case 20: glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, (i & 1) ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA); break;
			case 21: glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (i & 1) ? GL_LINEAR : GL_NEAREST); break;
			}
			if (regime == 11)
				glDrawArrays(GL_TRIANGLES, 0, 3);
			else if (regime == 25 || regime == 27)
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(i*sizeof(indices)));
			else if (regime == 26)
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
			else
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nil);
		}
		double t1 = BenchNow();
		glFinish();
		double t2 = BenchNow();
		host_logf("bench: %s: %.1f us a draw, %.1f with what the GPU had left to do", names[regime], (t1 - t0) / N, (t2 - t0) / N);
		// back to where the next one starts from
		if (regime == 8)
			glBindVertexArray(rw::gl3::defaultVertexArray);
		if (regime == 7 || regime == 12 || regime == 13) {
			glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
			glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
			BENCH_INPUT(0);
		}
		if (regime >= 23) {
			for (int i = 1; i >= 0; i--) {
				BENCH_INPUT(i);
				glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
				glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
			}
		}
		glBindTexture(GL_TEXTURE_2D, textures[0]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glUseProgram(program);
		glDisable(GL_BLEND);
		glDisable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		glDisable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
		glDepthMask(GL_FALSE);
	}
	#undef BENCH_INPUT
	glDeleteProgram(program2);
	glDeleteShader(vs2);
	glDeleteShader(fs2);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	if (vaos[0])
		glDeleteVertexArrays(2, vaos);
	glBindBuffer(GL_ARRAY_BUFFER, oldArray);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, oldElements);
	glDeleteBuffers(4, buffers);
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	glDeleteTextures(2, textures);
	glActiveTexture(oldActive);
	glUseProgram(oldProgram);
	glDeleteProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	if (oldBlend) glEnable(GL_BLEND);
	if (oldDepth) glEnable(GL_DEPTH_TEST);
	if (oldCull) glEnable(GL_CULL_FACE);
	glCullFace(oldCullFace);
	glDepthFunc(oldDepthFunc);
	glBlendFunc(oldBlendSrc, oldBlendDst);
	glDepthMask(oldDepthMask ? GL_TRUE : GL_FALSE);
	while (glGetError() != GL_NO_ERROR)
		;
}
