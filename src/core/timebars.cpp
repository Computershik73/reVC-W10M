#include "common.h"
#ifndef MASTER
#include "Font.h"
#include "Frontend.h"
#include "Timer.h"
#include "Text.h"

#define MAX_TIMERS (50)
#define MAX_MS_COLLECTED (40)

// enables frame time output
#define FRAMETIME

struct sTimeBar
{
	char name[20];
	float startTime;
	float endTime;
	int32 unk;
#ifdef GTA_W10M
	// librw's counts of draws while the timer ran (REVC_PERF=1): geometry and immediate
	uint32 draws, immediates;
#endif
};

struct
{
	sTimeBar Timers[MAX_TIMERS];
	uint32 count;
} TimerBar;
float MaxTimes[MAX_TIMERS];
float MaxFrameTime;

uint32 curMS;
uint32 msCollected[MAX_MS_COLLECTED];
#ifdef FRAMETIME
float FrameInitTime;
#endif

#ifdef GTA_W10M
static uint32 tbFrames;

// The timed parts of the frame last drawn and how long each took, for the phone's log
// (skel/w10m): the log is where a phone's frames can be looked into.
int32
tbLastFrame(uint32 *frame, const char **names, float *milliseconds, uint32 *draws, uint32 *immediates, int32 max)
{
	int32 n = 0;
	float perUnit = (float)CTimer::GetCyclesPerFrame() / (float)CTimer::GetCyclesPerMillisecond();
	*frame = tbFrames;
	for (uint32 i = 0; i < TimerBar.count && n < max; i++) {
		if (TimerBar.Timers[i].endTime < TimerBar.Timers[i].startTime)
			continue;
		names[n] = TimerBar.Timers[i].name;
		milliseconds[n] = (TimerBar.Timers[i].endTime - TimerBar.Timers[i].startTime) * perUnit;
		draws[n] = TimerBar.Timers[i].draws;
		immediates[n] = TimerBar.Timers[i].immediates;
		n++;
	}
	return n;
}
#endif

void tbInit()
{
#ifdef GTA_W10M
	tbFrames++;
#endif
	TimerBar.count = 0;
	uint32 i = CTimer::GetFrameCounter() & 0x7F;
	if (i == 0) {
		do
			MaxTimes[i++] = 0.0f;
		while (i != MAX_TIMERS);
#ifdef FRAMETIME
		MaxFrameTime = 0.0f;
#endif
	}
#ifdef FRAMETIME
	FrameInitTime = (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerFrame();
#endif
}

void tbStartTimer(int32 unk, Const char *name)
{
	strcpy(TimerBar.Timers[TimerBar.count].name, name);
	TimerBar.Timers[TimerBar.count].unk = unk;
	TimerBar.Timers[TimerBar.count].startTime = (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerFrame();
#ifdef GTA_W10M
	TimerBar.Timers[TimerBar.count].draws = rw::gl3::perf.draws;
	TimerBar.Timers[TimerBar.count].immediates = rw::gl3::perf.immediates;
#endif
	TimerBar.count++;
}

void tbEndTimer(Const char* name)
{
	uint32 n = 1500;
	for (uint32 i = 0; i < TimerBar.count; i++) {
		if (strcmp(name, TimerBar.Timers[i].name) == 0)
			n = i;
	}
	assert(n != 1500);
	TimerBar.Timers[n].endTime = (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerFrame();
#ifdef GTA_W10M
	TimerBar.Timers[n].draws = rw::gl3::perf.draws - TimerBar.Timers[n].draws;
	TimerBar.Timers[n].immediates = rw::gl3::perf.immediates - TimerBar.Timers[n].immediates;
#endif
}

float Diag_GetFPS()
{
	return 39000.0f / (msCollected[(curMS - 1) % MAX_MS_COLLECTED] - msCollected[curMS % MAX_MS_COLLECTED]);
}

void tbDisplay()
{
	char temp[200];
	wchar wtemp[200];

#ifdef FRAMETIME
	float FrameEndTime = (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerFrame();
#endif

	msCollected[(curMS++) % MAX_MS_COLLECTED] = RsTimer();
	CFont::SetBackgroundOff();
	CFont::SetBackgroundColor(CRGBA(0, 0, 0, 128));
	CFont::SetScale(0.48f, 1.12f);
	CFont::SetCentreOff();
	CFont::SetJustifyOff();
	CFont::SetWrapx(SCREEN_STRETCH_X(DEFAULT_SCREEN_WIDTH));
	CFont::SetRightJustifyOff();
	CFont::SetPropOn();
	CFont::SetFontStyle(FONT_STANDARD);
	sprintf(temp, "FPS: %.2f", Diag_GetFPS());
	AsciiToUnicode(temp, wtemp);
	CFont::SetColor(CRGBA(255, 255, 255, 255));
	if (!CMenuManager::m_PrefsMarketing || !CMenuManager::m_PrefsDisableTutorials) {
		CFont::PrintString(RsGlobal.maximumWidth * (4.0f / DEFAULT_SCREEN_WIDTH), RsGlobal.maximumHeight * (4.0f / DEFAULT_SCREEN_HEIGHT), wtemp);

#ifndef FINAL
		// Timers output (my own implementation)
		for (uint32 i = 0; i < TimerBar.count; i++) {
			MaxTimes[i] = Max(MaxTimes[i], TimerBar.Timers[i].endTime - TimerBar.Timers[i].startTime);
			sprintf(temp, "%s: %.2f", &TimerBar.Timers[i].name[0], MaxTimes[i]);
			AsciiToUnicode(temp, wtemp);
			CFont::PrintString(RsGlobal.maximumWidth * (4.0f / DEFAULT_SCREEN_WIDTH), RsGlobal.maximumHeight * ((8.0f * (i + 2)) / DEFAULT_SCREEN_HEIGHT), wtemp);
		}

#ifdef FRAMETIME
		MaxFrameTime = Max(MaxFrameTime, FrameEndTime - FrameInitTime);
		sprintf(temp, "Frame Time: %.2f", MaxFrameTime);
		AsciiToUnicode(temp, wtemp);

		CFont::PrintString(RsGlobal.maximumWidth * (4.0f / DEFAULT_SCREEN_WIDTH), RsGlobal.maximumHeight * ((8.0f * (TimerBar.count + 4)) / DEFAULT_SCREEN_HEIGHT), wtemp);
#endif // FRAMETIME
#endif // !FINAL
	}
}
#endif // !MASTER
