// Looking into where the GPU's time goes: nothing here is on in a game that is played.
//
// A frame that waits for the GPU (a Lumia 650 at anything over 640x360) says nothing about
// what the GPU is doing with its time. These settings change one thing about what it is asked
// for and leave the rest, or count what it is asked for:
//
// - REVC_D3D_VIEWPORT=60: the picture is drawn into the top left 60 per cent of its width and
//   height (36 per cent of its pixels), the rest stays black. The target, its clearing, its way
//   to the screen are those of the full size; the triangles' pixels are those of the smaller.
// - REVC_D3D_SCISSOR=50: nothing is drawn outside the top left 50 per cent of the width and the
//   height. The triangles are all there, and as big; a quarter of their pixels are drawn.
//   REVC_D3D_SCISSOR=1: next to none.
// - REVC_D3D_PIXELS=1: the pixels that are drawn are counted (Direct3D's occlusion queries),
//   for the whole frame and for each of the game's timed parts (core/timebars.cpp), and come
//   out in REVC_PERF's lines as so many pictures' worth. =2: with the depth test always
//   passing, which is every pixel a triangle covers, hidden or not (and a wrong picture).
//   The frame waits for the counts, and there is no drawing thread: for the scenes, not for
//   the frames a second.
// - REVC_D3D_DELAY=15: the drawing thread does nothing for 15 ms before it makes a frame's
//   calls. If the frames take 15 ms longer, the GPU draws a frame only once this thread has
//   handed all of it over, and the thread's calls and the GPU's work add up; if they take as
//   long as before, the GPU was at work meanwhile and is what a frame waits for.
// - The triangles asked for are counted in any case (REVC_PERF's lines have them).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rwbase.h"
#include "rwerror.h"
#include "rwplg.h"
#include "rwrender.h"
#include "rwengine.h"
#include "rwpipeline.h"
#include "rwobjects.h"

#include "rwgl3.h"
#include "rwgl3shader.h"
#include "rwgl3impl.h"

#include "d3d11impl.h"
#include "rwd3d11_host.h"

namespace rw {
namespace gl3 {

Measure measure;

enum { MAX_MARKS = 200 };
static ID3D11Query *queries[MAX_MARKS];
static int32 current;	// the query that is counting
static bool32 counting;
// the frame before
static uint64 lastPixels[MAX_MARKS];
static int32 numLast;

static int32
percent(const char *name)
{
	const char *value = getenv(name);
	int32 n = value ? atoi(value) : 0;
	return n > 0 && n < 100 ? n : 0;
}

void
openMeasure(void)
{
	measure.viewport = percent("REVC_D3D_VIEWPORT");
	measure.scissor = percent("REVC_D3D_SCISSOR");
	measure.pixels = percent("REVC_D3D_PIXELS");
	measure.delay = percent("REVC_D3D_DELAY");
	if(measure.delay)
		logf("d3d: REVC_D3D_DELAY, the drawing thread is kept busy for %d ms before each frame's calls", (int)measure.delay);
	if(measure.viewport)
		logf("d3d: REVC_D3D_VIEWPORT, the picture is drawn into %d per cent of its width and height", (int)measure.viewport);
	if(measure.scissor)
		logf("d3d: REVC_D3D_SCISSOR, nothing is drawn outside %d per cent of the picture's width and height", (int)measure.scissor);
	current = 0;
	counting = 0;
	numLast = 0;
	if(measure.pixels){
		D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
		for(int32 i = 0; i < MAX_MARKS; i++)
			if(FAILED(d3d.device->CreateQuery(&qd, &queries[i]))){
				logf("d3d: REVC_D3D_PIXELS, but this Direct3D counts no pixels");
				closeMeasure();
				measure.pixels = 0;
				return;
			}
		logf("d3d: REVC_D3D_PIXELS, the pixels drawn are counted%s; no drawing thread, and every frame waits for its count",
			measure.pixels == 2 ? ", with the depth test always passing" : "");
		d3d.context->Begin(queries[0]);
		counting = 1;
	}
}

void
closeMeasure(void)
{
	for(int32 i = 0; i < MAX_MARKS; i++)
		release(queries[i]);
	counting = 0;
}

// The frame's counts are taken, which waits for the GPU to have drawn it, and the next
// frame's begin
void
measureFrameEnd(void)
{
	if(!counting)
		return;
	d3d.context->End(queries[current]);
	d3d.context->Flush();
	uint64 t0 = perfNow();
	for(int32 i = 0; i <= current; i++){
		UINT64 n = 0;
		while(d3d.context->GetData(queries[i], &n, sizeof(n), 0) != S_OK){
			if(perfNow() - t0 > 2000000000ull){
				n = 0;
				break;
			}
			Sleep(0);
		}
		lastPixels[i] = n;
	}
	numLast = current + 1;
	current = 0;
	d3d.context->Begin(queries[0]);
}

}
}

using namespace rw;
using namespace rw::gl3;

// From here on the pixels are counted apart from those before: the number of this place in
// the frame, for rwd3d11_screens_between. 0 where nothing is counted.
extern "C" int
rwd3d11_pixel_mark(void)
{
	if(!counting || current + 1 >= MAX_MARKS)
		return current;
	d3d.context->End(queries[current]);
	current++;
	d3d.context->Begin(queries[current]);
	return current;
}

// The pixels drawn between two such places of the frame that was shown last, as so many
// pictures' worth; to < 0: up to the frame's end
extern "C" float
rwd3d11_screens_between(int from, int to)
{
	if(to < 0 || to > numLast)
		to = numLast;
	uint64 sum = 0;
	for(int32 i = from; i >= 0 && i < to; i++)
		sum += lastPixels[i];
	return d3d.width > 0 && d3d.height > 0 ? (float)((double)sum/((double)d3d.width*d3d.height)) : 0.0f;
}

extern "C" int
rwd3d11_counts_pixels(void)
{
	return counting ? measure.pixels : 0;
}

// the triangles asked for since the renderer started
extern "C" unsigned long long
rwd3d11_triangles(void)
{
	return measure.triangles;
}
