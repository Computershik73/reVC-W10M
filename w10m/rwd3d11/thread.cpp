// The drawing thread.
//
// On a GPU of Direct3D level 9 a frame's calls are most of what a frame costs (a Lumia 650
// driving: 15 ms of calls and up to 11 ms more to show the frame, of a frame of 35 to 40), and
// they need not be made where the game runs. The game's thread makes them on a deferred
// context, which only keeps them (4 microseconds a draw there); at the frame's end the list
// goes to this thread, which makes the calls on the device's own context and shows the frame,
// while the game's thread is on the next. The game is never more than one frame ahead: it
// hands a frame over when the one before has been shown.
//
// What does not go through the list:
// - Geometry written into the shared buffers (pipe.cpp). They are written where nothing is
//   drawn from, without discarding them, and a deferred context has no such write: the bytes
//   are kept here and written by this thread before it makes the frame's calls.
// - Reading anything back (a frame for REVC_FRAMESHOT, a texture's pixels) and a new size of
//   the swap chain: syncDevice() first, after which this thread does nothing and the game's
//   thread has the device's own context.
// - Making things (buffers, textures, shaders): the device takes that from any thread.
//
// A list of calls begins with nothing bound, so after every one handed over the renderer's
// idea of what is bound is forgotten (deviceStateLost).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

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

namespace rw {
namespace gl3 {

struct BufferWrite
{
	ID3D11Buffer *buffer;
	uint32 at, size;
	uint32 base;
	bool indices;
	size_t data;	// where its bytes begin in the frame's
};

struct Frame
{
	ID3D11CommandList *list;
	std::vector<BufferWrite> writes;
	std::vector<uint8> data;
	bool present;
	int32 interval;
	ID3D11DepthStencilView *depth;	// the picture's depth buffer, not to be kept once the frame is drawn
};

static std::thread drawingThread;
static std::mutex mutex;
static std::condition_variable wake, done;
static Frame handed;	// from the game's thread to this one
static bool hasFrame, busy, quit;
static ThreadPerf threadPerf;	// under the mutex

// of the frame the game's thread is on
static std::vector<BufferWrite> writes;
static std::vector<uint8> writeData;

static void
makeFrame(Frame &f)
{
	uint64 t0 = perfNow();
	for(BufferWrite &w : f.writes){
		D3D11_MAPPED_SUBRESOURCE mapped;
		HRESULT hr = d3d.immediate->Map(w.buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped);
		if(SUCCEEDED(hr)){
			const uint8 *from = &f.data[w.data];
			if(w.indices){
				// written in order, and never read: the memory may be the device's
				const uint16 *in = (const uint16*)from;
				uint16 *to = (uint16*)((uint8*)mapped.pData + w.at);
				for(uint32 i = 0; i < w.size/2; i++)
					to[i] = (uint16)(in[i] + w.base);
			}else
				memcpy((uint8*)mapped.pData + w.at, from, w.size);
			d3d.immediate->Unmap(w.buffer, 0);
		}else{
			static int said;
			if(said++ < 4)
				logf("d3d: a shared buffer could not be written on the drawing thread, 0x%08X", (unsigned)hr);
		}
		w.buffer->Release();
	}
	if(measure.delay && f.present){
		// measure.cpp: this thread kept busy for so long, to see whether the GPU waits for it
		uint64 until = perfNow() + (uint64)measure.delay*1000000ull;
		while(perfNow() < until)
			;
	}
	if(f.list){
		// the device's own context is left with nothing bound, as the next list expects
		d3d.immediate->ExecuteCommandList(f.list, FALSE);
		f.list->Release();
	}
	if(f.depth){
		if(d3d.immediate1)
			d3d.immediate1->DiscardView(f.depth);
		f.depth->Release();
	}
	uint64 t1 = perfNow();
	if(f.present){
		// Here the wait for the vertical blank is for the frames that come too quickly: the
		// menus, which nothing else holds back. A frame that comes a fortieth of a second or
		// more after the one before is the game at its own 30 a second, or slower, and waiting
		// for the blank as well held the game's thread up at the next frame (a Lumia 650: the
		// frame shown in 16 to 20 ms with the wait, 24 to 28 frames a second).
		static uint64 lastFrame;
		bool32 late = lastFrame && t0 - lastFrame >= 25000000ull;
		lastFrame = t0;
		presentFrame(f.interval, late);
	}
	uint64 t2 = perfNow();

	std::lock_guard<std::mutex> guard(mutex);
	threadPerf.makeNs += t1 - t0;
	threadPerf.showNs += t2 - t1;
	threadPerf.writes += (uint32)f.writes.size();
	if(f.present)
		threadPerf.frames++;
}

static void
drawing(void)
{
	// Above the other threads, as the OpenGL renderer's drawing thread is: a frame waits for
	// this one, and the game's own threads (the disc, the sound) may take the core from it
	// otherwise. REVC_D3D_PRIORITY=0: as any other thread.
	const char *priority = getenv("REVC_D3D_PRIORITY");
	if(priority == nil || atoi(priority) != 0)
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	for(;;){
		Frame f;
		{
			std::unique_lock<std::mutex> held(mutex);
			wake.wait(held, []{ return hasFrame || quit; });
			if(!hasFrame)
				return;
			f.list = handed.list;
			f.writes.swap(handed.writes);
			f.data.swap(handed.data);
			f.present = handed.present;
			f.interval = handed.interval;
			f.depth = handed.depth;
			handed.depth = nil;
			handed.list = nil;
			hasFrame = false;
			busy = true;
		}
		makeFrame(f);
		{
			std::lock_guard<std::mutex> guard(mutex);
			busy = false;
		}
		done.notify_all();
	}
}

bool32
startDrawingThread(void)
{
	ID3D11DeviceContext *deferred = nil;
	HRESULT hr = d3d.device->CreateDeferredContext(0, &deferred);
	if(FAILED(hr)){
		logf("d3d: no deferred context (0x%08X): the game's thread makes Direct3D's calls itself", (unsigned)hr);
		return 0;
	}
	d3d.immediate = d3d.context;
	d3d.context = deferred;
	d3d.threaded = 1;
	hasFrame = busy = quit = false;
	drawingThread = std::thread(drawing);
	return 1;
}

void
stopDrawingThread(void)
{
	if(!d3d.threaded)
		return;
	syncDevice();
	{
		std::lock_guard<std::mutex> guard(mutex);
		quit = true;
	}
	wake.notify_all();
	drawingThread.join();
	d3d.context->Release();
	d3d.context = d3d.immediate;
	d3d.threaded = 0;
}

void
submitFrame(bool32 present, int32 interval, ID3D11DepthStencilView *depth)
{
	if(depth)
		depth->AddRef();
	ID3D11CommandList *list = nil;
	HRESULT hr = d3d.context->FinishCommandList(FALSE, &list);
	if(FAILED(hr)){
		static int said;
		if(said++ < 4)
			logf("d3d: the frame's calls could not be handed over, 0x%08X", (unsigned)hr);
		list = nil;
	}
	{
		std::unique_lock<std::mutex> held(mutex);
		uint64 t0 = perfNow();
		done.wait(held, []{ return !hasFrame && !busy; });
		threadPerf.waitedNs += perfNow() - t0;
		handed.list = list;
		handed.writes.swap(writes);
		handed.data.swap(writeData);
		handed.present = present != 0;
		handed.interval = interval;
		handed.depth = depth;
		hasFrame = true;
	}
	wake.notify_one();
	// what came back in the exchange is of a frame already made
	writes.clear();
	writeData.clear();
	deviceStateLost();
}

void
syncDevice(void)
{
	if(!d3d.threaded)
		return;
	submitFrame(0, 0);
	std::unique_lock<std::mutex> held(mutex);
	done.wait(held, []{ return !hasFrame && !busy; });
}

void
queueBufferWrite(ID3D11Buffer *buffer, uint32 at, uint32 size, const void *data, const uint32 *base)
{
	BufferWrite w;
	w.buffer = buffer;
	w.at = at;
	w.size = size;
	w.base = base ? *base : 0;
	w.indices = base != nil;
	w.data = writeData.size();
	buffer->AddRef();
	writeData.insert(writeData.end(), (const uint8*)data, (const uint8*)data + size);
	writes.push_back(w);
}

void
takeThreadPerf(ThreadPerf *p)
{
	std::lock_guard<std::mutex> guard(mutex);
	*p = threadPerf;
	p->shows = showsCounted;
	p->showsAtOnce = showsAtOnceCounted;
	showsCounted = showsAtOnceCounted = 0;
	memset(&threadPerf, 0, sizeof(threadPerf));
}

}
}
