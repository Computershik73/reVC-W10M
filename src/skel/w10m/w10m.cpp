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

#include "AutoPilot.h"
#include "Camera.h"
#include "CarCtrl.h"
#include "CivilianPed.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "General.h"
#include "Pad.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Pools.h"
#include "Population.h"
#include "Streaming.h"
#include "Vehicle.h"
#include "WeaponInfo.h"
#include "World.h"
#include "Renderer.h"
#include "RwHelper.h"
#include "Timer.h"
#include "main.h"
#include "postfx.h"

#include <direct.h>

#include "host_api.h"
#ifdef RW_D3D11
#include "rwd3d11_host.h"
#endif

int32 tbLastFrame(uint32 *frame, const char **names, float *milliseconds, uint32 *draws, uint32 *immediates,
                  uint32 *triangles, float *screens, int32 max);	// core/timebars.cpp
extern float W10M_VehicleDetail;	// rw/VisibilityPlugins.cpp
#ifdef RW_D3D11
extern bool W10M_SkyAfterWorld;	// core/main.cpp
#endif
bool ReadIniIfExists(const char *cat, const char *key, uint8 *out);	// core/re3.cpp
bool glt_on(void);	// w10m/glthread: OpenGL is called on a drawing thread
void StoreIni(const char *cat, const char *key, uint8 val);

// Where a frame's time goes, every ten seconds, into the log (next to the GLFW layer's own
// line with the frames a second and the swap): the game's timed parts, as its debug timers
// have them. In a menu there are none.
static void
NoteFrameParts(void)
{
	enum { MAX_PARTS = 40 };
	static char partNames[MAX_PARTS][20];
	static float partSums[MAX_PARTS];
	static uint32 partDraws[MAX_PARTS], partImmediates[MAX_PARTS], partTriangles[MAX_PARTS];
	static float partScreens[MAX_PARTS], frameScreens;
	static unsigned long long trianglesNoted;
	static int32 numParts, numFrames;
	static uint32 lastFrame, noted;

	const char *names[MAX_PARTS];
	float ms[MAX_PARTS];
	uint32 draws[MAX_PARTS], immediates[MAX_PARTS], triangles[MAX_PARTS];
	float screens[MAX_PARTS];
	uint32 frame;
	int32 n = tbLastFrame(&frame, names, ms, draws, immediates, triangles, screens, MAX_PARTS);
	rw::gl3::perf.frame++;
	if (frame != lastFrame) {
		lastFrame = frame;
		numFrames++;
#ifdef RW_D3D11
		frameScreens += rwd3d11_screens_between(0, -1);
#endif
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
				partDraws[j] = partImmediates[j] = partTriangles[j] = 0;
				partScreens[j] = 0.0f;
				numParts++;
			}
			partSums[j] += ms[i];
			partDraws[j] += draws[i];
			partImmediates[j] += immediates[i];
			partTriangles[j] += triangles[i];
			partScreens[j] += screens[i];
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
	// which part draws how much (REVC_PERF=1): draws of geometry and immediate ones, a frame
	if (rw::gl3::perf.enabled && numFrames > 0 && numParts > 0) {
		char text[900];
		int32 used = snprintf(text, sizeof(text), "perf: draws a frame in the game's parts, of geometry + immediate:");
		for (int32 j = 0; j < numParts && used < (int32)sizeof(text) - 48; j++)
			if (partDraws[j] + partImmediates[j] >= (uint32)numFrames / 2)
				used += snprintf(text + used, sizeof(text) - used, " %s %.0f+%.0f;", partNames[j],
				                 (float)partDraws[j] / numFrames, (float)partImmediates[j] / numFrames);
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
		// and by the pipeline that made it, with those that took over a millisecond each
		if (!glt_on()) {
			static const char *pipes[4] = { "default", "default from atlases", "material effects", "skin" };
			char text[600];
			int32 used = snprintf(text, sizeof(text), "perf: draw calls by pipeline, a frame:");
			for (int32 i = 0; i < 4 && used < (int32)sizeof(text) - 110; i++)
				if (p.pipeDraws[i])
					used += snprintf(text + used, sizeof(text) - used, " %s %.0f, %.1f ms (%.1f of them over a millisecond, %.1f ms);",
					                 pipes[i], p.pipeDraws[i] * f, p.pipeNs[i] * 1e-6f * f, p.pipeLong[i] * f,
					                 p.pipeLongNs[i] * 1e-6f * f);
			snprintf(text + used, sizeof(text) - used, " immediate draws over a millisecond %.1f, %.1f ms",
			         p.immediateLong * f, p.immediateLongNs * 1e-6f * f);
			host_log(text);
		}
		for (int32 i = 0; i < 4; i++) {
			p.pipeDraws[i] = p.pipeLong[i] = 0;
			p.pipeNs[i] = p.pipeLongNs[i] = 0;
		}
		p.immediateLong = 0;
		p.immediateLongNs = 0;
		if (rw::gl3::gl3Caps.atlases)
			host_logf("perf: atlases: %.0f meshes a frame went into the draw of the mesh before them", p.merged * f);
		p.merged = 0;
#ifdef RW_D3D11
		char own[900];
		rwd3d11_perf_line(own, sizeof(own), numFrames);
		host_logf("perf: Direct3D: %s", own);
		// what the GPU is asked for (w10m/rwd3d11/measure.cpp): the triangles, and with
		// REVC_D3D_PIXELS the pixels, as so many pictures' worth
		{
			char text[900];
			unsigned long long all = rwd3d11_triangles();
			int32 used = snprintf(text, sizeof(text), "perf: %.1f thousand triangles a frame; in the game's parts:",
			                      (double)(all - trianglesNoted) * 1e-3 * f);
			trianglesNoted = all;
			for (int32 j = 0; j < numParts && used < (int32)sizeof(text) - 40; j++)
				if (partTriangles[j] * f >= 50.0f)
					used += snprintf(text + used, sizeof(text) - used, " %s %.1f;", partNames[j], partTriangles[j] * 1e-3f * f);
			host_log(text);
			if (rwd3d11_counts_pixels()) {
				used = snprintf(text, sizeof(text), "perf: %.2f screens of pixels a frame%s; in the game's parts:", frameScreens * f,
				                rwd3d11_counts_pixels() == 2 ? ", hidden ones too (no depth test)" : " are drawn");
				for (int32 j = 0; j < numParts && used < (int32)sizeof(text) - 40; j++)
					if (partScreens[j] * f >= 0.005f)
						used += snprintf(text + used, sizeof(text) - used, " %s %.2f;", partNames[j], partScreens[j] * f);
				host_log(text);
			}
		}
#endif
		p.draws = p.immediates = 0;
		p.inputNs = p.stateNs = p.drawNs = p.immediateNs = 0;
		p.programs = p.uniforms = p.textures = p.samplers = p.vertexArrays = p.blends = p.depths = p.culls = 0;
	}
	numParts = numFrames = 0;
	frameScreens = 0.0f;
}

// What a draw costs here (REVC_BENCH=1): through ANGLE, and through Direct3D 11 itself
extern "C" void W10M_DrawBenchmark(void);	// w10m/bench/glbench.cpp
extern "C" void W10M_Direct3DBenchmark(void);	// w10m/bench/d3dbench.cpp

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
#ifdef RW_D3D11
	// the Direct3D 11 renderer (w10m/rwd3d11): a GPU of Direct3D's level 9 is the slow kind.
	// REVC_SPARE=1 or 0: this phone is treated as one, or is not, whatever its level (to see
	// on one phone what the treatment is worth apart from the level).
	bool slow = rwd3d11_feature_level() < 100;
	if (const char *spare = getenv("REVC_SPARE"))
		slow = atoi(spare) != 0;
	host_logf("w10m: Direct3D feature level %d.%d, shared buffers %s, atlases %s%s", rwd3d11_feature_level() / 10,
	          rwd3d11_feature_level() % 10, rw::gl3::gl3Caps.sharedBuffers ? "yes" : "no",
	          rw::gl3::gl3Caps.atlases ? "yes" : "no", slow ? "; sparing a slow phone" : "");
#else
	bool slow = rw::gl3::gl3Caps.gles && rw::gl3::gl3Caps.glversion < 30;
	host_logf("w10m: OpenGL ES %d.%d, vertex arrays %s, program binaries %s, immediate mode's vertices from %s, "
	          "shared buffers %s, atlases %s%s",
	          rw::gl3::gl3Caps.glversion / 10, rw::gl3::gl3Caps.glversion % 10,
	          rw::gl3::gl3Caps.vertexArrays ? "yes" : "no",
	          rw::gl3::gl3Caps.programBinaries ? (rw::gl3::programCacheDir ? "kept" : "not kept") : "no",
	          rw::gl3::gl3Caps.verticesFromMemory ? "memory" : "buffers",
	          rw::gl3::gl3Caps.sharedBuffers ? "yes" : "no", rw::gl3::gl3Caps.atlases ? "yes" : "no",
	          slow ? "; sparing a slow phone" : "");
#endif
	// REVC_PS2_ALPHA=1 or 0: the PlayStation 2's alpha test (the display menu's setting, which
	// draws everything that blends twice, once for the depth buffer) on or off for this run,
	// on any phone. The game saves it with its settings when it next saves them.
	const char *ps2 = getenv("REVC_PS2_ALPHA");
	if (ps2)
		gPS2alphaTest = atoi(ps2) != 0;
	if (!slow) {
		// REVC_PROGRAMS=few: the same on a phone that is not slow, to see what it is worth there
		const char *programs = getenv("REVC_PROGRAMS");
		rw::gl3::fewerPrograms = programs != nil && strcmp(programs, "few") == 0;
		return;
	}
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
#ifdef RW_D3D11
	// The clouds after the world's houses, so that what a house hides of them is not drawn
	// (core/main.cpp, RenderScene). REVC_SKY_FIRST=1: before them, as the game has it.
	W10M_SkyAfterWorld = getenv("REVC_SKY_FIRST") == nil;
#endif
	// Vehicles in full detail up to 30 m instead of 70. REVC_FULL_DETAIL=1 keeps the 70.
	if (getenv("REVC_FULL_DETAIL") == nil)
		W10M_VehicleDetail = 0.43f;
	// Three of the game's own settings, set once (its display menu has them, and what is
	// chosen there stays): the colour filter copies the whole picture into a texture every
	// frame, which ANGLE on this kind of GPU does in 50 ms at 1280x720, so its plain variant,
	// which does not; the draw distance at its shortest; and the PlayStation 2's alpha test
	// off, the PC game's own single pass instead of two draws for everything that blends.
	// (That one was set at every start once, whatever the menu said, and was saved with the
	// settings of a phone that only tried REVC_SPARE=1 for a run: an Elite x3 had it so.)
	// REVC_KEEP_SETTINGS=1: none of them, for a run that is to leave the game's settings alone.
	uint8 spared = 0;
	if (getenv("REVC_KEEP_SETTINGS") == nil && (!ReadIniIfExists("W10M", "SlowPhoneDefaults", &spared) || !spared)) {
		if (ps2 == nil)
			gPS2alphaTest = false;
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

// A run that drives itself (REVC_KEYS=...,d@30): from then on the player's car is driven as
// the traffic's cars are, along the roads and round what is on them, and a player who has no
// car is put behind the wheel of the nearest. Keys held by the clock (87@60-65.5) end in a
// wall as soon as the game is a little quicker or slower than where they were timed.
static bool gDriveItself;

extern "C" void
reVC_W10M_DriveItself(void)
{
	gDriveItself = true;
}

static void
DriveItself(void)
{
	CPlayerPed *player = FindPlayerPed();
	if (player == nil || gGameState != GS_PLAYING_GAME || CCutsceneMgr::IsRunning() ||
	    FrontEndMenuManager.GetIsMenuActive() || player->DyingOrDead())
		return;

	if (!player->InVehicle()) {
		// once a second: the nearest car nobody drives, within 40 m
		static uint32 looked;
		uint32 now = CTimer::GetTimeInMilliseconds();
		if (now - looked < 1000)
			return;
		looked = now;
		CVehicle *nearest = nil;
		float best = 40.0f * 40.0f;
		for (int32 i = CPools::GetVehiclePool()->GetSize() - 1; i >= 0; i--) {
			CVehicle *v = CPools::GetVehiclePool()->GetSlot(i);
			if (v == nil || !v->IsCar() || v->pDriver || v->GetStatus() == STATUS_WRECKED)
				continue;
			float d = (v->GetPosition() - player->GetPosition()).MagnitudeSqr();
			if (d < best) {
				best = d;
				nearest = v;
			}
		}
		if (nearest) {
			player->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, nearest);
			player->WarpPedIntoCar(nearest);
			host_log("w10m: the run drives itself: the player is put into the nearest car");
		}
		return;
	}

	CVehicle *car = player->m_pMyVehicle;
	if (car == nil || car->pDriver != player || !car->IsCar())
		return;
	if (car->GetStatus() != STATUS_PHYSICS || car->AutoPilot.m_nCarMission != MISSION_CRUISE) {
		CCarCtrl::JoinCarWithRoadSystem(car);
		car->AutoPilot.m_nCarMission = MISSION_CRUISE;
		car->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
		car->AutoPilot.m_nCruiseSpeed = 20;
		car->AutoPilot.m_nAntiReverseTimer = CTimer::GetTimeInMilliseconds();
		car->SetStatus(STATUS_PHYSICS);
		car->bEngineOn = true;
		host_log("w10m: the run drives itself: the player's car goes with the traffic");
	}
}

// A run that tries the aim (REVC_AIM_TEST=standard, classic or classic-free): the control
// method of the game's controls menu as named, with the display menu's free camera for
// "-free" ("-still": the right stick is left alone), for this run alone (put back at the end; nothing saves it meanwhile). Once the
// player has had his controls on foot for three seconds he gets a pistol, and two men stand
// still 7 m ahead of him, 2.5 m to his left and to his right. Then the pad is worked by the
// clock (reVC_W10M_TestPad): the right stick right at 0.6 (3-5 s) and at full (5-7 s), R1
// held (8-12.5 s) with the circle at 9 s, R2 at 10 s and the circle at 11 s, the circle alone
// at 14 s. Every quarter of a second the camera, the player and his target go into the log
// ("aim:" lines); the test ends at 16 s.
static int32 gAimTest = -1;	// -1 REVC_AIM_TEST not read; 0 off; 1 waiting for the player; 2 running; 3 done
static bool gAimClassic, gAimFree, gAimStill;
static int8 gAimWasMethod;
static bool gAimWasMouse, gAimWasFree;
static uint32 gAimOnFoot, gAimStart, gAimLogged;
static int32 gAimPeds[2] = { -1, -1 };

static CPed *
AimTestPed(int32 i)
{
	return gAimPeds[i] < 0 ? nil : CPools::GetPedPool()->GetAt(gAimPeds[i]);
}

static float
AimTestSeconds(void)
{
	return (CTimer::GetTimeInMilliseconds() - gAimStart) / 1000.0f;
}

static void
AimTestStandingMan(CPlayerPed *player, int32 i, float side)
{
	CVector pos = player->GetPosition() + player->GetForward() * 7.0f + player->GetRight() * side;
	CPed *ped = new CCivilianPed(PEDTYPE_CIVMALE, MI_MALE01);
	ped->CharCreatedBy = MISSION_CHAR;
	ped->bRespondsToThreats = false;
	ped->bAllowMedicsToReviveMe = false;
	ped->SetPosition(pos);
	float facing = CGeneral::LimitRadianAngle(player->GetForward().Heading() + PI);
	ped->SetHeading(facing);
	ped->m_fRotationCur = ped->m_fRotationDest = facing;
	CWorld::Add(ped);
	CPopulation::ms_nTotalMissionPeds++;
	gAimPeds[i] = CPools::GetPedPool()->GetIndex(ped);
}

static const char *
AimTestTargetName(CEntity *target)
{
	if (target == nil)
		return "none";
	if (target == AimTestPed(0))
		return "left";
	if (target == AimTestPed(1))
		return "right";
	return "other";
}

static void
AimTestLog(CPlayerPed *player, const char *what)
{
	CCam &cam = TheCamera.Cams[TheCamera.ActiveCam];
	const char *process = cam.Mode != CCam::MODE_FOLLOWPED ? "other" :
	                      CCamera::m_bUseMouse3rdPerson ? "mouse" : CCamera::bFreeCam ? "rotation" : "classic";
	float camHeading = RADTODEG(CVector2D(cam.Front.x, cam.Front.y).Heading());
	CPed *left = AimTestPed(0), *right = AimTestPed(1);
	host_logf("aim: %5.2f s %s camera mode %d (%s) heading %4.0f, player heading %4.0f state %d weapon %d ammo %d, target %s, men's health %.0f %.0f",
	          AimTestSeconds(), what, cam.Mode, process, camHeading, RADTODEG(player->m_fRotationCur), player->m_nPedState,
	          player->GetWeapon()->m_eWeaponType, player->GetWeapon()->m_nAmmoTotal, AimTestTargetName(player->m_pPointGunAt),
	          left ? left->m_fHealth : -1.0f, right ? right->m_fHealth : -1.0f);
}

static void
AimTest(void)
{
	if (gAimTest < 0) {
		const char *mode = getenv("REVC_AIM_TEST");
		gAimTest = mode ? 1 : 0;
		if (mode) {
			gAimClassic = strncmp(mode, "classic", 7) == 0;
			gAimFree = strstr(mode, "free") != nil;
			gAimStill = strstr(mode, "still") != nil;
		}
	}
	if (gAimTest != 1 && gAimTest != 2)
		return;
	CPlayerPed *player = FindPlayerPed();
	if (player == nil || gGameState != GS_PLAYING_GAME || CCutsceneMgr::IsRunning() ||
	    FrontEndMenuManager.GetIsMenuActive() || player->InVehicle() || CPad::GetPad(0)->ArePlayerControlsDisabled()) {
		gAimOnFoot = 0;
		return;
	}
	uint32 now = CTimer::GetTimeInMilliseconds();
	if (gAimTest == 1) {
		if (gAimOnFoot == 0)
			gAimOnFoot = now;
		if (now - gAimOnFoot < 3000)
			return;
		gAimWasMethod = FrontEndMenuManager.m_ControlMethod;
		gAimWasMouse = CCamera::m_bUseMouse3rdPerson;
		gAimWasFree = CCamera::bFreeCam;
		FrontEndMenuManager.m_ControlMethod = gAimClassic ? CONTROL_CLASSIC : CONTROL_STANDARD;
		CCamera::m_bUseMouse3rdPerson = !gAimClassic;
		CCamera::bFreeCam = gAimFree;

		CStreaming::RequestModel(MI_MALE01, STREAMFLAGS_DONT_REMOVE);
		CStreaming::RequestModel(MI_COLT45, STREAMFLAGS_DONT_REMOVE);
		CStreaming::LoadAllRequestedModels(false);
		player->GiveWeapon(WEAPONTYPE_COLT45, 200);
		player->m_nSelectedWepSlot = CWeaponInfo::GetWeaponInfo(WEAPONTYPE_COLT45)->m_nWeaponSlot;
		player->SetCurrentWeapon(WEAPONTYPE_COLT45);
		AimTestStandingMan(player, 0, -2.5f);
		AimTestStandingMan(player, 1, 2.5f);
		gAimStart = gAimLogged = now;
		gAimTest = 2;
		host_logf("aim: the test begins: control method %s, free camera %s (were %d, %d)",
		          gAimClassic ? "classic" : "standard", gAimFree ? "on" : "off", gAimWasMethod, gAimWasFree);
		AimTestLog(player, "start");
		return;
	}
	if (now - gAimLogged >= 250) {
		gAimLogged = now;
		AimTestLog(player, "     ");
	}
	if (AimTestSeconds() >= 16.0f) {
		FrontEndMenuManager.m_ControlMethod = gAimWasMethod;
		CCamera::m_bUseMouse3rdPerson = gAimWasMouse;
		CCamera::bFreeCam = gAimWasFree;
		gAimTest = 3;
		host_log("aim: the test is over, the control method and the free camera are as they were");
	}
}

// The GLFW layer's pad (w10m/glfw), after the shell's state is in it: what REVC_AIM_TEST has
// the pad do at this point of its clock. Buttons and axes in GLFW's gamepad order.
extern "C" void
reVC_W10M_TestPad(float *axes, unsigned char *buttons)
{
	if (gAimTest != 2)
		return;
	float t = AimTestSeconds();
	if (gAimStill)
		;
	else if (t >= 3.0f && t < 5.0f)
		axes[2] = 0.6f;	// the right stick, to the right
	else if (t >= 5.0f && t < 7.0f)
		axes[2] = 1.0f;
	if (t >= 8.0f && t < 12.5f)
		buttons[5] = 1;	// R1
	if ((t >= 9.0f && t < 9.15f) || (t >= 11.0f && t < 11.15f) || (t >= 14.0f && t < 14.15f))
		buttons[1] = 1;	// the circle
	if (t >= 10.0f && t < 10.15f)
		axes[5] = 1.0f;	// R2
}

extern "C" int32
reVC_W10M_Frame(void)
{
	int32 context = CurrentContext();
	int32 requests = host_take_requests();
	if (gSkipCutscenes)
		requests |= HOST_REQUEST_SKIP_CUTSCENE;
	if (gDriveItself)
		DriveItself();
	AimTest();

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
		if (getenv("REVC_BENCH")) {
#ifndef RW_D3D11
			W10M_DrawBenchmark();
#endif
			W10M_Direct3DBenchmark();
		}
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
