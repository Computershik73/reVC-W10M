// Windows 10 Mobile: what the game tells the UWP shell and takes from it, apart from the
// window and the input, which go through GLFW's API (w10m/glfw, skel/glfw/glfw.cpp).
//
// The shell draws the on-screen controls and has to know which ones fit (a menu, on foot, in
// a vehicle, a cutscene) and whose names to put on them: the controller type of the game's
// own menu, which is also what the game's prompts are drawn with. It has a few things to ask
// for that are not button presses.
// Called once a frame, on the game thread, from glfwPollEvents().
#ifdef GTA_W10M

#include "common.h"
#include "crossplatform.h"

#include "CutsceneMgr.h"
#include "Frontend.h"
#include "General.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Renderer.h"
#include "RwHelper.h"
#include "Timer.h"
#include "main.h"
#include "postfx.h"

#include <direct.h>

#include "host_api.h"

int32 tbLastFrame(uint32 *frame, const char **names, float *milliseconds, int32 max);	// core/timebars.cpp
extern float W10M_VehicleDetail;	// rw/VisibilityPlugins.cpp
bool ReadIniIfExists(const char *cat, const char *key, uint8 *out);	// core/re3.cpp
bool glt_on(void);	// w10m/glthread: OpenGL is called on a drawing thread
void StoreIni(const char *cat, const char *key, uint8 val);

// Where a frame's time goes, every ten seconds, into the log (next to the GLFW layer's own
// line with the frames a second and the swap): the game's timed parts, as its debug timers
// have them. In a menu there are none.
static void
NoteFrameParts(void)
{
	enum { MAX_PARTS = 24 };
	static char partNames[MAX_PARTS][20];
	static float partSums[MAX_PARTS];
	static int32 numParts, numFrames;
	static uint32 lastFrame, noted;

	const char *names[MAX_PARTS];
	float ms[MAX_PARTS];
	uint32 frame;
	int32 n = tbLastFrame(&frame, names, ms, MAX_PARTS);
	rw::gl3::perf.frame++;
	if (frame != lastFrame) {
		lastFrame = frame;
		numFrames++;
		for (int32 i = 0; i < n; i++) {
			int32 j;
			for (j = 0; j < numParts; j++)
				if (strcmp(partNames[j], names[i]) == 0)
					break;
			if (j == numParts) {
				if (numParts == MAX_PARTS)
					continue;
				strncpy(partNames[j], names[i], sizeof(partNames[j]) - 1);
				partSums[j] = 0.0f;
				numParts++;
			}
			partSums[j] += ms[i];
		}
	}

	uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
	if (noted == 0)
		noted = now;
	if (now - noted < 10000)
		return;
	noted = now;
	if (numFrames > 0 && numParts > 0) {
		// some are parts of others (the scene's of RenderScene): there is no sum to give
		char text[900];
		int32 used = snprintf(text, sizeof(text), "perf: ms a frame in the game's parts:");
		for (int32 j = 0; j < numParts && used < (int32)sizeof(text) - 40; j++)
			if (partSums[j] / numFrames >= 0.5f)
				used += snprintf(text + used, sizeof(text) - used, " %s %.0f", partNames[j], partSums[j] / numFrames);
		host_log(text);
	}
	// and what the drawing itself costs (REVC_PERF=1): librw's count of the calls and of the
	// time in them
	if (rw::gl3::perf.enabled && numFrames > 0) {
		rw::gl3::Perf &p = rw::gl3::perf;
		float f = 1.0f / numFrames;
		// with a drawing thread the times are what it costs to queue the calls: the calls
		// themselves are in the GLFW layer's line about that thread
		host_logf("perf: %.0f draws a frame: vertex input %.1f ms, state and uniforms %.1f ms, the draw calls %.1f ms; "
		          "%.0f immediate draws, %.1f ms%s",
		          p.draws * f, p.inputNs * 1e-6f * f, p.stateNs * 1e-6f * f, p.drawNs * 1e-6f * f,
		          p.immediates * f, p.immediateNs * 1e-6f * f, glt_on() ? " (to queue them)" : "");
		host_logf("perf: changed between them, a frame: %.0f programs, %.0f uniforms, %.0f textures (%.0f different ones), "
		          "%.0f sampler settings, %.0f vertex arrays, %.0f blend, %.0f depth, %.0f culling",
		          p.programs * f, p.uniforms * f, p.textures * f, p.differentTextures * f, p.samplers * f,
		          p.vertexArrays * f, p.blends * f, p.depths * f, p.culls * f);
		p.differentTextures = 0;
		// with the calls on the game's own thread only: what a draw call takes there by
		// what was changed before it
		if (!glt_on()) {
			static const char *after[8] = {
				"nothing of these", "a texture", "the vertex array", "texture and vertex array",
				"the program", "program and texture", "program and vertex array", "all three",
			};
			char text[600];
			int32 used = snprintf(text, sizeof(text), "perf: a draw call by what was changed before it, us (and how many a frame):");
			for (int32 i = 0; i < 8 && used < (int32)sizeof(text) - 60; i++)
				if (p.drawsAfter[i])
					used += snprintf(text + used, sizeof(text) - used, " %s %.0f (%.0f);", after[i],
					                 p.drawNsAfter[i] * 1e-3f / p.drawsAfter[i], p.drawsAfter[i] * f);
			host_log(text);
		}
		for (int32 i = 0; i < 8; i++) {
			p.drawsAfter[i] = 0;
			p.drawNsAfter[i] = 0;
		}
		if (rw::gl3::gl3Caps.atlases)
			host_logf("perf: atlases: %.0f meshes a frame went into the draw of the mesh before them", p.merged * f);
		p.merged = 0;
		p.draws = p.immediates = 0;
		p.inputNs = p.stateNs = p.drawNs = p.immediateNs = 0;
		p.programs = p.uniforms = p.textures = p.samplers = p.vertexArrays = p.blends = p.depths = p.culls = 0;
	}
	numParts = numFrames = 0;
}

// ===[ What a draw costs here (REVC_BENCH=1) ]===
// The same small quad drawn a few hundred times, with one thing changed between the draws at
// a time: what this phone's driver, and ANGLE in front of it, charge for a draw as such and
// for each kind of change. Once, when the menu first comes up; the lines go to the log.

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

static void
DrawBenchmark(void)
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
	static const uint16 indices[6] = { 0, 1, 2, 0, 2, 3 };
	static const float identity[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
	static float big[64*4];
	static uint8 pixels[4*4*4];
	static uint8 scratch[360000];

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
	static uint16 manyIndices[N*6];
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
					manyIndices[i*6 + j] = (uint16)(i*4 + indices[j]);
			}
			BENCH_INPUT(1);
			glBufferData(GL_ARRAY_BUFFER, sizeof(manyVerts), manyVerts, GL_STATIC_DRAW);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(manyIndices), manyIndices, GL_STATIC_DRAW);
		}
		for (int i = 0; i < N; i++) {
			float f = (i & 1) ? 1.0f : 0.99f;
			uint16 moved[6];
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
					moved[j] = (uint16)(i*4 + indices[j]);
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
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)(uintptr)(i*sizeof(indices)));
			else if (regime == 26)
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
			else
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nil);
		}
		double t1 = BenchNow();
		glFinish();
		double t2 = BenchNow();
		host_logf("bench: %s: %.0f us a draw, %.0f with what the GPU had left to do", names[regime], (t1 - t0) / N, (t2 - t0) / N);
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

// Before the engine starts (psInitialize)
void
W10M_Start(void)
{
	// Compiled shader programs are kept in the app's own folder: compiling them is over a
	// minute of every start on a phone with a Direct3D level 9 GPU (librw, gl3shader.cpp)
	static char cache[512];
	const wchar_t *local = host_local_folder();
	if (local && getenv("REVC_NO_SHADER_CACHE") == nil) {
		wchar_t folder[400];
		_snwprintf(folder, 400, L"%ls\\shaders", local);
		folder[399] = 0;
		_wmkdir(folder);
		size_t n = wcstombs(cache, folder, sizeof(cache) - 1);
		if (n != (size_t)-1 && n > 0)
			rw::gl3::programCacheDir = cache;
	}
	rw::gl3::perf.enabled = getenv("REVC_PERF") != nil;
}

// What a slow phone is spared, once the engine has started and says what it runs on.
// OpenGL ES 2.0 here means ANGLE on a Direct3D level 9 GPU, and a processor to match: a
// drawing call costs several times what it costs elsewhere, and there are a thousand a frame.
static void
SpareTheSlow(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	bool slow = rw::gl3::gl3Caps.gles && rw::gl3::gl3Caps.glversion < 30;
	host_logf("w10m: OpenGL ES %d.%d, vertex arrays %s, program binaries %s, immediate mode's vertices from %s, "
	          "shared buffers %s, atlases %s%s",
	          rw::gl3::gl3Caps.glversion / 10, rw::gl3::gl3Caps.glversion % 10,
	          rw::gl3::gl3Caps.vertexArrays ? "yes" : "no",
	          rw::gl3::gl3Caps.programBinaries ? (rw::gl3::programCacheDir ? "kept" : "not kept") : "no",
	          rw::gl3::gl3Caps.verticesFromMemory ? "memory" : "buffers",
	          rw::gl3::gl3Caps.sharedBuffers ? "yes" : "no", rw::gl3::gl3Caps.atlases ? "yes" : "no",
	          slow ? "; sparing a slow phone" : "");
	if (!slow) {
		// REVC_PROGRAMS=few: the same on a phone that is not slow, to see what it is worth there
		const char *programs = getenv("REVC_PROGRAMS");
		rw::gl3::fewerPrograms = programs != nil && strcmp(programs, "few") == 0;
		return;
	}
	// The PlayStation 2's alpha test draws everything that blends twice (once for the depth
	// buffer): the PC game's own single pass instead. REVC_PS2_ALPHA=1 keeps it.
	if (getenv("REVC_PS2_ALPHA") == nil)
		gPS2alphaTest = false;
	// One program for a pipeline, not the cheapest of four for every mesh: changing programs
	// is the dearest thing between two draws here (librw, fewerPrograms). REVC_PROGRAMS=all
	// keeps the four.
	// Of the four it is the two with the alpha test that are used, by whether the atomic has
	// lights: buildings have none, and are spared the lights' arithmetic for every vertex.
	// REVC_PROGRAMS=few: the one with the lights for everything. =noat: nothing is
	// discarded, which is a measurement and not a picture.
	const char *programs = getenv("REVC_PROGRAMS");
	rw::gl3::fewerPrograms = 2;
	if (programs && strcmp(programs, "all") == 0)
		rw::gl3::fewerPrograms = 0;
	if (programs && strcmp(programs, "few") == 0)
		rw::gl3::fewerPrograms = 1;
	if (programs && strcmp(programs, "noat") == 0)
		rw::gl3::fewerPrograms = 3;
	// Vehicles in full detail up to 30 m instead of 70. REVC_FULL_DETAIL=1 keeps the 70.
	if (getenv("REVC_FULL_DETAIL") == nil)
		W10M_VehicleDetail = 0.43f;
	// Two of the game's own settings, set once (its display menu has them, and what is
	// chosen there stays): the colour filter copies the whole picture into a texture every
	// frame, which ANGLE on this kind of GPU does in 50 ms at 1280x720, so its plain variant,
	// which does not; and the draw distance at its shortest.
	uint8 spared = 0;
	if (!ReadIniIfExists("W10M", "SlowPhoneDefaults", &spared) || !spared) {
		if (CPostFX::EffectSwitch == CPostFX::POSTFX_NORMAL || CPostFX::EffectSwitch == CPostFX::POSTFX_MOBILE)
			CPostFX::EffectSwitch = CPostFX::POSTFX_SIMPLE;
		FrontEndMenuManager.m_PrefsLOD = 0.925f;
		CRenderer::ms_lodDistScale = FrontEndMenuManager.m_PrefsLOD;
		StoreIni("W10M", "SlowPhoneDefaults", (uint8)1);
		FrontEndMenuManager.SaveSettings();
	}
}

static int32
CurrentContext(void)
{
	if (FrontEndMenuManager.GetIsMenuActive())
		return HOST_CONTEXT_MENU;
	if (gGameState != GS_PLAYING_GAME)
		return HOST_CONTEXT_LOADING;
	if (CCutsceneMgr::IsRunning())
		return HOST_CONTEXT_CUTSCENE;

	CPlayerPed *player = FindPlayerPed();
	if (player == nil)
		return HOST_CONTEXT_LOADING;
	return player->InVehicle() ? HOST_CONTEXT_VEHICLE : HOST_CONTEXT_ON_FOOT;
}

static int32
CurrentPadStyle(void)
{
	switch (FrontEndMenuManager.m_PrefsControllerType) {
	case CMenuManager::CONTROLLER_DUALSHOCK2:
	case CMenuManager::CONTROLLER_DUALSHOCK3:
	case CMenuManager::CONTROLLER_DUALSHOCK4:
		return HOST_PAD_STYLE_PLAYSTATION;
	case CMenuManager::CONTROLLER_NINTENDO_SWITCH:
		return HOST_PAD_STYLE_NINTENDO;
	default:
		return HOST_PAD_STYLE_XBOX;
	}
}

// The pad's radio buttons. The radio (cMusicManager) takes the step when it next looks, which
// is not every frame the game draws; a step asked for where no radio could take it (out of a
// vehicle, in a police car's) is not a change of station the next time one can.
static int32 gRadioStep;
static uint32 gRadioStepTime;

int32
W10M_TakeRadioStep(void)
{
	int32 step = gRadioStep;
	gRadioStep = 0;
	return CTimer::GetTimeInMillisecondsPauseMode() - gRadioStepTime < 500 ? step : 0;
}

// for a run nobody holds the phone for (REVC_KEYS, the GLFW layer)
static bool gSkipCutscenes;

extern "C" void
reVC_W10M_SkipCutscenes(void)
{
	gSkipCutscenes = true;
}

extern "C" int32
reVC_W10M_Frame(void)
{
	int32 context = CurrentContext();
	int32 requests = host_take_requests();
	if (gSkipCutscenes)
		requests |= HOST_REQUEST_SKIP_CUTSCENE;

	// The skip button ends the cutscene itself instead of pressing Cross for a frame:
	// CCutsceneMgr::Update() looks at the pad only in some camera modes and load states, and a
	// press that lands on another frame is lost (found by the Android port). The finale is
	// not to be skipped there either.
	if ((requests & HOST_REQUEST_SKIP_CUTSCENE) && CCutsceneMgr::IsRunning()
	    && CGeneral::faststricmp(CCutsceneMgr::GetCutsceneName(), "finale"))
		CCutsceneMgr::FinishCutscene();

	// Another app, the lock screen or a call came up: the phone suspends the process within
	// seconds, and the player comes back to the pause menu, not to a car that kept going.
	if ((requests & HOST_REQUEST_PAUSE) && context != HOST_CONTEXT_MENU && context != HOST_CONTEXT_LOADING)
		FrontEndMenuManager.RequestFrontEndStartUp();

	if (requests & (HOST_REQUEST_RADIO_NEXT | HOST_REQUEST_RADIO_PREVIOUS)) {
		gRadioStep = (requests & HOST_REQUEST_RADIO_NEXT) ? 1 : -1;
		gRadioStepTime = CTimer::GetTimeInMillisecondsPauseMode();
	}

	// The pad's own button for the names on it: PlayStation's or Xbox's, the same setting as
	// Controller type in the game's controller menu (which has the Switch's too). In a menu
	// only: that is where the button is, and where the textures it changes are loaded.
	if ((requests & HOST_REQUEST_NEXT_PAD_STYLE) && context == HOST_CONTEXT_MENU) {
		int8 type = CurrentPadStyle() == HOST_PAD_STYLE_PLAYSTATION ? CMenuManager::CONTROLLER_XBOXONE
		                                                            : CMenuManager::CONTROLLER_DUALSHOCK2;
		FrontEndMenuManager.m_PrefsControllerType = type;
		FrontEndMenuManager.LoadController(type);
		FrontEndMenuManager.SaveSettings();
	}

	host_set_context(context);
	host_set_pad_style(CurrentPadStyle());
	SpareTheSlow();
	NoteFrameParts();
	static bool benched;
	if (!benched && context == HOST_CONTEXT_MENU) {
		benched = true;
		if (getenv("REVC_BENCH"))
			DrawBenchmark();
	}

	// what the driver has compiled since goes to the kept programs too (other variants of a
	// program, at the first draws that need them)
	static uint32 saved;
	uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
	if (now - saved >= 20000) {
		saved = now;
		rw::gl3::saveProgramBinaries();
	}
	return context;
}

#endif
