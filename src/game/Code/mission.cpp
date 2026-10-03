//******************************************************************************************
//	mission.cpp - This file contains the mission class code
//		Missions are what scenarios were in MechCommander 1.
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//----------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include "mclib.h"
#endif

#include "missiongui.h"
#ifndef MISSION_H
#include "mission.h"
#include "devtools.h"
#endif

//#ifndef MOVE_H
//#include "move.h"
//#endif

#ifndef MOVEMGR_H
#include "movemgr.h"
#endif

#ifndef GAMESOUND_H
#include "gamesound.h"
#endif

#ifndef SOUNDS_H
#include "Sounds.h" // PORT: match case-sensitive host path.
#endif

#ifndef COLLSN_H
#include "Collsn.h" // PORT: match case-sensitive host path.
#endif

#ifndef CMPONENT_H
#include "cmponent.h"
#endif

#ifndef OBJMGR_H
#include "objmgr.h"
#endif

#ifndef MOVER_H
#include "mover.h"
#endif

#ifndef MECH_H
#include "mech.h"
#endif

#ifndef GVEHICL_H
#include "gvehicl.h"
#endif

#ifndef CONTACT_H
#include "contact.h"
#endif

#ifndef TEAM_H
#include "Team.h" // PORT: match case-sensitive host path.
#endif

#ifndef COMNDR_H
#include "comndr.h"
#endif

#ifndef GROUP_H
#include "group.h"
#endif

#ifndef GAMECAM_H
#include "gamecam.h"
#endif

#ifndef MULTPLYR_H
#include "multplyr.h"
#endif

#ifndef WEAPONFX_H
#include "weaponfx.h"
#endif

#ifndef WEATHER_H
#include "weather.h"
#endif

#ifndef GATE_H
#include "gate.h"
#endif

#ifndef LOGISTICSDATA_H
#include "LogisticsData.h" // PORT: match case-sensitive host path.
#endif

#ifndef LOGISTICSPILOT_H
#include "LogisticsPilot.h" // PORT: match case-sensitive host path.
#endif

#ifndef GAMELOG_H
#include "gamelog.h"
#endif

#ifndef CELLIP_H
#include "cellip.h"
#endif

#ifndef PREFS_H
#include "prefs.h"
#endif

#include "../resource.h" // PORT: use host-independent include separators.

#include <GameOS.HPP> // PORT: match case-sensitive host path.
#include <mc2_port.h> // PORT: MC2_TeardownCheckpoint, the teardown census breadcrumb.

#include <chrono>   // PORT (B1-3): the Mission::update phase split below.

//----------------------------------------------------------------------------------
// PORT (B1-3): THE PHASE SPLIT OF Mission::update.
//
// WHAT IS ALREADY MEASURED, so nobody re-derives it. MC2_SLOW_FRAME_MS was armed
// for the owner's 2026-08-21 session and split the FRAME into logic and render
// over 1,120,238 frames: 662 of the 690 reported slow frames were logic-
// dominated, 28 render-dominated, and the worst frame spent 1,989 ms of its
// 2,007 ms inside DoGameLogic. Effect DRAWING -- the original hunch -- is dead
// by measurement. Do not go back to it.
//
// WHAT THE SHAPE OF THAT DATA SAYS, and it is not "an allocation spike":
// re-reading the 690 reports, 12 of them are RUNS of 26 to 33 CONSECUTIVE slow
// frames, each run totalling 930-1,050 ms of logic at a sustained 30-40 ms per
// frame while renderMs stays near 0.8 ms throughout. Two further events (ticks
// 2538 and 3124) are single ~2,000 ms frames and look like mission load. So
// there are at least two different things in here, and an instrument that only
// reported averages would hide both.
//
// This splits the phase the way the frame was split, at the boundaries retail
// already named with ProfileTime -- plus the big calls retail left unwrapped
// (terrainTextures, ObjectManager::update, missionInterface::updateVTol).
//
// MC2_LOGIC_PHASE_MS=<ms> arms it. Unset or 0 is OFF and reads no clock at all,
// so the strict bar cannot move. The per-phase session totals print at exit
// whether or not any turn crossed the threshold, because a threshold that was
// never crossed is itself an answer.
//
// THIS PROBE CAN COME BACK NEGATIVE: if the slow turns spread evenly across
// every phase, the cost is in none of them and the next instrument goes
// somewhere else entirely.
//----------------------------------------------------------------------------------
enum MC2LogicPhaseId
{
	MC2_PHASE_INTERFACE = 0,
	MC2_PHASE_CAMERA,
	MC2_PHASE_VTOL,
	MC2_PHASE_TERRAIN,
	MC2_PHASE_WEATHER,
	MC2_PHASE_WAYPOINTS,
	MC2_PHASE_PATHMGR,
	MC2_PHASE_TERRAINTEX,
	MC2_PHASE_GEOMETRY,
	MC2_PHASE_OBJECTS,
	MC2_PHASE_CRATERS,
	MC2_PHASE_TXMMGR,
	MC2_PHASE_SENSORS,
	MC2_PHASE_COLLISIONS,
	MC2_PHASE_ABL,
	MC2_PHASE_COUNT
};

static const char* const MC2_PhaseNames[MC2_PHASE_COUNT] =
{
	"interface", "camera", "vtol", "terrain", "weather", "waypoints",
	"pathMgr", "terrainTex", "geometry", "objects", "craters", "txmMgr",
	"sensors", "collisions", "abl"
};

static double	MC2_PhaseThresholdMs	= -1.0;		// -1 = env not read yet
static double	MC2_PhaseNow[MC2_PHASE_COUNT];		// this turn
static double	MC2_PhaseTotal[MC2_PHASE_COUNT];	// whole session
static double	MC2_PhaseWorstBreak[MC2_PHASE_COUNT];
static double	MC2_PhaseWorstMs		= 0.0;
static long		MC2_PhaseWorstTurn		= -1;
static long		MC2_PhaseSlowTurns		= 0;
static long		MC2_PhaseTurns			= 0;
static double	MC2_PhaseSlowMsTotal	= 0.0;
static bool		MC2_PhaseSummaryHooked	= false;

static void MC2_PhaseSummary (void)
{
	if (MC2_PhaseThresholdMs <= 0.0)
		return;

	double sessionTotal = 0.0;
	for (long i = 0; i < MC2_PHASE_COUNT; i++)
		sessionTotal += MC2_PhaseTotal[i];

	printf("[logic-phase] armed at %.1f ms over %ld turns: %ld slow turns, "
	       "%.0f ms of logic in them, %.0f ms of phase time overall\n",
	       MC2_PhaseThresholdMs, MC2_PhaseTurns, MC2_PhaseSlowTurns,
	       MC2_PhaseSlowMsTotal, sessionTotal);

	for (long i = 0; i < MC2_PHASE_COUNT; i++)
		printf("[logic-phase]   %-11s total=%9.1f ms  (%5.1f%% of measured phase time)\n",
		       MC2_PhaseNames[i], MC2_PhaseTotal[i],
		       sessionTotal > 0.0 ? (100.0 * MC2_PhaseTotal[i] / sessionTotal) : 0.0);

	if (MC2_PhaseWorstTurn >= 0)
	{
		printf("[logic-phase] worst turn=%ld totalMs=%.2f\n",
		       MC2_PhaseWorstTurn, MC2_PhaseWorstMs);
		for (long i = 0; i < MC2_PHASE_COUNT; i++)
			if (MC2_PhaseWorstBreak[i] > 0.01)
				printf("[logic-phase]   worst %-11s %8.2f ms\n",
				       MC2_PhaseNames[i], MC2_PhaseWorstBreak[i]);
	}
	fflush(stdout);
}

static inline bool MC2_PhaseArmed (void)
{
	if (MC2_PhaseThresholdMs < 0.0)
	{
		const char* value = getenv("MC2_LOGIC_PHASE_MS");
		MC2_PhaseThresholdMs = (value && *value) ? atof(value) : 0.0;
		if (MC2_PhaseThresholdMs > 0.0 && !MC2_PhaseSummaryHooked)
		{
			MC2_PhaseSummaryHooked = true;
			atexit(MC2_PhaseSummary);
			printf("[logic-phase] armed at %.1f ms -- splitting Mission::update "
			       "into %d phases\n", MC2_PhaseThresholdMs, (int)MC2_PHASE_COUNT);
			fflush(stdout);
		}
	}
	return MC2_PhaseThresholdMs > 0.0;
}

static inline void MC2_PhaseAdd (long id, double ms)
{
	MC2_PhaseNow[id]   += ms;
	MC2_PhaseTotal[id] += ms;
}

// Wraps one phase. When the probe is off this expands to the bare call, exactly
// as retail's ProfileTime does outside LAB_ONLY, and no clock is read.
#define MC2_PHASE(id, ...)                                                       \
	do {                                                                         \
		if (MC2_PhaseArmed())                                                    \
		{                                                                        \
			const std::chrono::steady_clock::time_point mc2PhaseT0 =             \
				std::chrono::steady_clock::now();                                \
			__VA_ARGS__;                                                         \
			MC2_PhaseAdd((id), std::chrono::duration<double, std::milli>(        \
				std::chrono::steady_clock::now() - mc2PhaseT0).count());         \
		}                                                                        \
		else { __VA_ARGS__; }                                                    \
	} while (0)

// Scoped so it reports on EVERY exit from Mission::update, including the early
// mission-over returns.
struct MC2PhaseTurn
{
	long turn;
	bool armed;

	MC2PhaseTurn (long t) : turn(t), armed(MC2_PhaseArmed())
	{
		if (!armed) return;
		for (long i = 0; i < MC2_PHASE_COUNT; i++)
			MC2_PhaseNow[i] = 0.0;
	}

	~MC2PhaseTurn (void)
	{
		if (!armed) return;

		double total = 0.0;
		for (long i = 0; i < MC2_PHASE_COUNT; i++)
			total += MC2_PhaseNow[i];

		MC2_PhaseTurns++;

		if (total > MC2_PhaseWorstMs)
		{
			MC2_PhaseWorstMs = total;
			MC2_PhaseWorstTurn = turn;
			for (long i = 0; i < MC2_PHASE_COUNT; i++)
				MC2_PhaseWorstBreak[i] = MC2_PhaseNow[i];
		}

		if (total < MC2_PhaseThresholdMs)
			return;

		MC2_PhaseSlowTurns++;
		MC2_PhaseSlowMsTotal += total;

		// One line per slow turn carrying every phase, so a RUN of slow turns
		// can be read as a block instead of reconstructed from a summary --
		// the runs are the whole shape of this defect.
		char line[1024];
		int  used = snprintf(line, sizeof(line),
			"[logic-phase] turn=%ld totalMs=%.2f", turn, total);
		for (long i = 0; i < MC2_PHASE_COUNT && used > 0 && used < (int)sizeof(line); i++)
		{
			if (MC2_PhaseNow[i] <= 0.01)
				continue;
			used += snprintf(line + used, sizeof(line) - used, " %s=%.2f",
			                 MC2_PhaseNames[i], MC2_PhaseNow[i]);
		}
		fprintf(stderr, "%s\n", line);
		fflush(stderr);
	}
};

//----------------------------------------------------------------------------------
// Macro Definitions
//#define NUM_FIRERANGES		3

//----------------------------------------------------------------------------------
// Static globals
float minFrameRate = 4.0;
float minFrameLength = 1.0/4.0;

long MissionStartTime =	0;			//No Idea

Mission *mission = NULL;
unsigned long scenarioResult = mis_PLAYING;
long scenarioEndTurn = -1;

extern long GameDifficulty;
long MechSalvageChance = 100;

long globalPlayerSkills[4];		//Per spec.  Integer value = percentage * 100
long globalPlayerWeapons[4];
long globalEnemySkills[4];
long globalEnemyWeapons[4];

extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
extern float OptimalRangePoints[NUM_WEAPON_RANGE_TYPES];
extern bool OptimalRangePointInRange[NUM_WEAPON_RANGE_TYPES][3];

float globalMissionValues[MAX_GLOBAL_MISSION_VALUES];

extern Stuff::Vector3D debugMechActorPosition[];
extern float mechDebugAngle[];
extern float torsoDebugAngle[];

extern Stuff::Vector3D debugGVActorPosition;

extern GameObjectFootPrint* tempSpecialAreaFootPrints;
extern long tempNumSpecialAreas;
extern DWORD ServerPlayerNum;

extern bool useNonWeaponEffects;

extern unsigned long elementHeapSize;
extern unsigned long maxElements;
extern unsigned long maxGroups;
extern unsigned long missionHeapSize;
extern unsigned long polyHeapSize;
extern unsigned long spriteDataHeapSize;
extern unsigned long spriteHeapSize;

extern long	CurMultiplayCode;
extern long	CurMultiplayParam;

extern bool quitGame;
extern float MaxExtractUnitDistance;

extern bool useFog;
extern bool useShadows;
extern bool inViewMode;
extern unsigned long viewObject;
extern float loadProgress;

// PORT (M8-B49): present a frame at each progress waypoint. Without this the whole
// load happens inside one logic tick and the bar is never drawn until it already
// reads 100 -- which is exactly what the instrument reported.
extern void MC2_PumpLoadFrame();

extern char TeamRelations[MAX_TEAMS][MAX_TEAMS];

ByteFlag *VisibleBits = NULL;		//What can currently be seen
ByteFlag *SeenBits = NULL;			//What HAS been seen

UserHeapPtr missionHeap = NULL;

unsigned long MultiPlayTeamId = 0xFFFFFFFF;
unsigned long MultiPlayCommanderId = 0xFFFFFFFF;

bool useSensors = true;

// PORT (M8-B45): DOES THE SENSOR SYSTEM ACTUALLY RUN?
//
// Measured on the owner's own mc2_15 quicksave, headless: 24,000 enemy-vehicle
// evaluations and every single one read CONTACT_NONE -- including enemies 36 m
// from a player unit. Two explanations fit that equally well and they lead in
// opposite directions: either the sensor system is genuinely failing to spot
// (the owner's "there was nothing on the tac map"), or the headless harness
// never runs it and the whole measurement is an artifact of the harness.
//
// The gate below is what separates them, so it is measured rather than reasoned
// about: how many times SensorManager->update() was actually called, how many
// times the gate refused, and which of the three conditions did the refusing.
// A run that reports calls=0 indicts the harness; a run that reports thousands
// of calls and still no contacts indicts the sensor system.
//
// File-scope and read once, deliberately: a getenv or a function-local static in
// a per-frame path moved the strict bar's hash before (R1-19).
static const bool MC2_SensorGateCensus = []() {
	const char* e = getenv("MC2_GV_FACE_CENSUS");
	return e && *e && strcmp(e, "0") != 0;
}();
static long MC2_SensorUpdateCalls = 0;
static long MC2_SensorGateSkips = 0;
static bool MC2_SensorGateAnnounced = false;
bool useCollisions = true;
long missionLineChanged = 0;
bool GeneralAlarm = false;

extern bool KillAmbientLight;

extern GameLog* CombatLog;
// PORT (2026-08-21): MC2 ships a developer key set that this build compiled
// out (CMakeLists defines FINAL). Owner ruling: expose the functions, but as
// a PANEL rather than a wall of ctrl+alt+shift chords -- INSERT opens it. The
// panel lives in devtools.cpp; only the one thing it cannot reach from there,
// the scenario result, is applied here.
//
// NEVER-ENDING STORY IS DELIBERATELY GONE. It suppressed win, loss,
// objectives AND the time limit with no way to turn it back off, so a single
// press stranded the campaign. A tool nobody can undo is a trap, not a tool.

// Read from gamesys.fit below whether or not the panel is armed: one float,
// meaningless until something uses it.
float CheatHitDamage = 0.0f;


void GetBlockedDoorCells (long moveLevel, long door, char* openCells);
void PlaceStationaryMovers (MoveMap* map);
void PlaceMovers (void);
//---------------------------------------------------------------------------
void initABL (void);
void closeABL (void);

#define	MAX_DISABLE_AT_START	100
extern long NumDisableAtStart;
extern long DisableAtStart[MAX_DISABLE_AT_START];
bool showFrameRate = false;

bool Mission::terminationCounterStarted = false;
double Mission::missionTerminationTime = -1.0;
unsigned long Mission::terminationResult = mis_PLAYING;

extern float OneOverProcessorSpeed;
extern PriorityQueuePtr	openList;

//
// Macro used for statistic timing of main functions
//
#ifdef LAB_ONLY
#define ProfileTime(x,y)	x=GetCycles();y;x=GetCycles()-x;
extern __int64 MCTimeTerrainUpdate 	;
extern __int64 MCTimeCameraUpdate 		;
extern __int64 MCTimeWeatherUpdate 	;
extern __int64 MCTimePathManagerUpdate ; 
extern __int64 MCTimeRunBrainUpdate ;
extern __int64 MCTimePath1Update ;
extern __int64 MCTimePath2Update ;
extern __int64 MCTimePath3Update ;
extern __int64 MCTimePath4Update ;
extern __int64 MCTimePath5Update ;
extern __int64 MCTimeCalcGoal1Update ;
extern __int64 MCTimeCalcPath1Update;
extern __int64 MCTimeCalcPath2Update;
extern __int64 MCTimeCalcPath3Update;
extern __int64 MCTimeCalcPath4Update;
extern __int64 MCTimeCalcPath5Update;
extern __int64 MCTimeCalcGoal2Update ;
extern __int64 MCTimeCalcGoal3Update ;
extern __int64 MCTimeCalcGoal4Update ;
extern __int64 MCTimeCalcGoal5Update ;
extern __int64 MCTimeCalcGoal6Update ;
extern __int64 MCTimeTerrainGeometry 	; 
extern __int64 MCTimeCraterUpdate 		; 
extern __int64 MCTimeTXMManagerUpdate 	; 
extern __int64 MCTimeSensorUpdate 		; 
extern __int64 MCTimeLOSUpdate			; 
extern __int64 MCTimeCollisionUpdate 	; 
extern __int64 MCTimeMissionScript 	; 
extern __int64 MCTimeInterfaceUpdate 	; 
extern __int64 MCTimeMissionTotal; 

extern __int64 MCTimeLOSCalc;
extern __int64 MCTimeTerrainObjectsUpdate;
extern __int64 MCTimeMechsUpdate;
extern __int64 MCTimeVehiclesUpdate;
extern __int64 MCTimeTurretsUpdate;

extern __int64 MCTimeTerrainObjectsTL;
extern __int64 MCTimeMechsTL;
extern __int64 MCTimeVehiclesTL;
extern __int64 MCTimeTurretsTL;

extern __int64 MCTimeAllElseUpdate;

extern __int64 MCTimeAnimationCalc;

extern __int64 MCTimeABLLoad 			;
extern __int64 MCTimeMiscToTeamLoad 	; 
extern __int64 MCTimeTeamLoad 			; 
extern __int64 MCTimeObjectLoad 		; 
extern __int64 MCTimeTerrainLoad 		; 
extern __int64 MCTimeMoveLoad 			; 
extern __int64 MCTimeMissionABLLoad 	; 
extern __int64 MCTimeWarriorLoad 		; 
extern __int64 MCTimeMoverPartsLoad	; 
extern __int64 MCTimeObjectiveLoad 	; 
extern __int64 MCTimeCommanderLoad 	; 
extern __int64 MCTimeMiscLoad 			; 
extern __int64 MCTimeGUILoad 			; 

extern __int64 x1;

extern __int64 x;


#else
#define ProfileTime(x,y)	y;
#endif

long GameVisibleVertices		= 60;
float BaseHeadShotElevation		= 1.0f;

bool DisplayCameraAngle = false;
extern long MaxResourcePoints;
extern long resolution;
extern long renderer;

bool loadInMissionSave = false;

//***************************************************************************
// PORT: B1-1 (M8-B7). Issue the owner's order from the soak.
//
// Every crash of theirs followed the same action -- select the second unit,
// order it to guard the first -- and the unattended soak had only ever loaded
// and watched. This drives the SAME function the mouse reaches
// (MissionInterfaceManager::handleOrders, via the body of ::doGuard) rather
// than a synthetic order path, because an order the GUI would never build
// would test the wrong thing.
//
// Silent and harmless when there are fewer than two home movers.
//***************************************************************************
//***************************************************************************
// M8-B12, 2026-08-20. The whole chain from the shipped .txm to the texture
// handed to the GPU has been verified correct, so the remaining question is
// what the DRAW does with it -- and that needs a rendered frame with a road
// in it. The game can be run headless and a frame captured, but the camera
// starts wherever the mission drops it, and on mc2_08 that view contains no
// road at all: the first capture came back with nothing to look at.
//
// MC2_CAMERA_GOTO_ROAD=<n> jumps the camera to the n-th map cell that carries
// a road overlay, once, at tick 120 (late enough that the mission is live and
// the camera has been aimed by the game first, so this overrides a settled
// state rather than racing it).
//
// THIS IS A LOOKING GLASS, NOT A FIX, and it must never be left armed: it
// moves the player camera. Off unless the variable is set, and it prints the
// cell and world position it chose so a capture can be tied to a place on the
// map.
//
// If it finds no road cell it says so. That is a result too -- it would mean
// the map carries no overlay where one was expected, which is a different
// fault from a road that draws wrongly.
//***************************************************************************
void MC2_CameraGotoRoad(long tick)
{
	static int wanted = -1;
	static bool done = false;
	if (wanted < 0) {
		const char* want = getenv("MC2_CAMERA_GOTO_ROAD");
		wanted = (want && *want) ? atol(want) : 0;
	}
	if (wanted <= 0 || done || tick < 120)
		return;
	if (!GameMap || !eye)
		return;
	done = true;

	const long side = Terrain::realVerticesMapSide * MAPCELL_DIM;
	long found = 0;

	// Report the encoding before trusting it. Cell 0,0 came back non-zero on the
	// first run, which is either a real overlay at the map corner or a value this
	// code is reading wrongly -- and jumping the camera on that guess would have
	// produced a screenshot of nothing with a confident label on it.
	{
		long hist[64];
		memset(hist, 0, sizeof(hist));
		long nonZero = 0;
		for (long r = 0; r < side; r++)
			for (long c = 0; c < side; c++)
			{
				const unsigned long v = GameMap->getOverlay(r, c);
				if (v) nonZero++;
				if (v < 64) hist[v]++;
			}
		printf("[cam-goto] overlay histogram over %ld x %ld cells, nonZero=%ld:\n",
			       side, side, nonZero);
		for (long v = 0; v < 64; v++)
			if (hist[v]) printf("[cam-goto]    value %2ld : %ld cells\n", v, hist[v]);
		printf("[cam-goto]    road cells (values 0-2) = %ld\n",
			       hist[0] + hist[1] + hist[2]);
		fflush(stdout);
	}
	for (long r = 0; r < side; r++)
	{
		for (long c = 0; c < side; c++)
		{
			// 3 IS THE EMPTY VALUE, NOT 0, and the histogram above is what
			// said so: 123,264 of 129,600 cells carry 3, against 4,779 / 567
			// / 990 for 0 / 1 / 2. The field is two bits wide -- a movement
			// overlay class, not the full Overlays enum -- so 0 is a real
			// road type (DIRT_ROAD) and treating it as "none" sent the first
			// run to the map corner with a confident label on it.
			if (GameMap->getOverlay(r, c) >= 3)
				continue;
			if (++found < wanted)
				continue;
			Stuff::Vector3D world;
			land->cellToWorld(r, c, world);
			printf("[cam-goto] road %ld at cell %ld,%ld -> world %.1f,%.1f,%.1f\n",
				       found, r, c, world.x, world.y, world.z);
			fflush(stdout);
			eye->setPosition(world, false);
			return;
		}
	}
	printf("[cam-goto] NO ROAD CELL FOUND in %ld x %ld cells -- the map carries "
		       "no overlay, which is not the same fault as a road drawing wrong\n",
		       side, side);
	fflush(stdout);
}

//***************************************************************************
// PORT (M8-B15): put the camera at an EXACT state, headlessly, so a view the
// owner screenshotted can be reproduced without spending their time again.
//
// MC2_CAMERA_PRESET="angle,rotation,x,y"  -- e.g. the failing shot 20 of the
// 2026-08-22 session is "20,-72.125,195.804352,5008.803223".
//
// Reapplied EVERY tick after tick 120, because the camera's own update pulls
// the projection angle and rotation back toward their goals otherwise, and a
// preset that silently drifts would be measured as if it had held.
//
// LIKE MC2_CAMERA_GOTO_ROAD, THIS MOVES THE PLAYER CAMERA. It must never be
// left armed in the launcher. Default is off and it reads no environment more
// than once.
//***************************************************************************
void MC2_CameraPreset(long tick)
{
	static int   parsed = -1;
	static float wantAngle = 0.0f, wantRot = 0.0f, wantX = 0.0f, wantY = 0.0f;
	static bool  announced = false;

	if (parsed < 0)
	{
		parsed = 0;
		const char* want = getenv("MC2_CAMERA_PRESET");
		if (want && *want)
		{
			float a = 0, r = 0, x = 0, y = 0;
			if (sscanf(want, "%f,%f,%f,%f", &a, &r, &x, &y) == 4)
			{
				wantAngle = a; wantRot = r; wantX = x; wantY = y;
				parsed = 1;
			}
			else
			{
				printf("[cam-preset] could not parse '%s' -- expected "
				       "angle,rotation,x,y\n", want);
				fflush(stdout);
			}
		}
	}

	// Count our OWN calls rather than trusting `turn`. Mission::load restores
	// the saved turn, so a run that loads a supplied save sits at turn 50 for
	// the whole capture window and a `turn >= 120` gate never fires -- which
	// silently produced four "different" rotations that were pixel-identical
	// because the camera had never been touched.
	static long calls = 0;
	calls++;
	if (!parsed || calls < 60 || !eye)
		return;

	Stuff::Vector3D world;
	world.x = wantX;
	world.y = wantY;
	world.z = 0.0f;
	if (land)
		world.z = land->getTerrainElevation(world);

	eye->setPosition(world, false);
	// setCameraRotation sets the YAW pair, NOT the projection angle -- the
	// first version of this preset passed the angle there and every run came
	// back at 18 degrees regardless of what was asked for, which would have
	// been read as "the angle does not matter". tiltValue is the real setter.
	eye->setCameraRotation(wantRot, wantRot);
	eye->tiltValue(wantAngle);

	if (!announced)
	{
		announced = true;
		// PORT: print what STUCK, not what was asked. tiltValue clamps, and a
		// preset that reports its request would read as if the clamp had not
		// fired. `held` is the number to believe.
		printf("[cam-preset] asked angle=%.3f held=%.3f rotation=%.3f at world "
		       "%.3f,%.3f from tick %ld -- THIS MOVES THE PLAYER CAMERA\n",
		       wantAngle, eye->getProjectionAngle(), wantRot, wantX, wantY, tick);
		fflush(stdout);
	}
}

//***************************************************************************
void MC2_SoakIssueGuardOrder(unsigned long cycle)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char* want = getenv("MC2_SOAK_GUARD_ORDER");
		enabled = (want && *want && strcmp(want, "0") != 0) ? 1 : 0;
		if (enabled)
			printf("[soak] guard orders armed: unit 2 guards unit 1 before every load\n");
	}
	if (!enabled)
		return;

	MissionInterfaceManager* mim = MissionInterfaceManager::instance();
	Team* pTeam = Team::home;
	if (!mim || !pTeam || !Commander::home)
		return;

	Mover* first = NULL;
	Mover* second = NULL;
	for (long i = 0; i < pTeam->getRosterSize(); i++) {
		Mover* m = (Mover*)pTeam->getMover(i);
		if (!m || !m->getCommander() ||
			m->getCommander()->getId() != Commander::home->getId())
			continue;
		if (!first) first = m;
		else if (!second) { second = m; break; }
	}
	if (!first || !second)
		return;

	// handleOrders acts on the SELECTED movers, so the selection is the order's
	// real argument. Restore it afterwards: leaving the soak's selection behind
	// would change what the next tick does for reasons unrelated to the test.
	const bool firstWas = first->isSelected();
	const bool secondWas = second->isSelected();
	first->setSelected(false);
	second->setSelected(true);

	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_GUARD);
	tacOrder.targetWID = first->getWatchID();
	tacOrder.pack(NULL, NULL);
	mim->handleOrders(tacOrder);

	first->setSelected(firstWas);
	second->setSelected(secondWas);

	printf("[soak] cycle %lu: unit 2 ordered to guard unit 1\n", cycle);
	fflush(stdout);
}
bool saveInMissionSave = false;

float forcedFrameRate = -1.0f;

extern bool 			invulnerableON;		//Used for tutorials so mechs can take damage, but look like they are taking damage!  Otherwise, I'd just use NOPAIN!!

#define DEFAULT_SKY			1
//----------------------------------------------------------------------------------
// class Mission
long Mission::update (void)
{
	if (active)
	{
		turn++;
		MC2_PublishedTurn = turn;   // PORT (M8-B18): see mc2_port.h

		// PORT (B1-3): scoped so the phase breakdown is reported on every exit
		// from this function, including the mission-over returns below.
		MC2PhaseTurn mc2PhaseTurn(turn);

		// PORT: deterministic A2 save/load round-trip harness. The interactive
		// quick-save failure took a long campaign run to reach; these opt-in turn
		// gates exercise the identical Mission::save/load path headlessly.
		static long portSaveAtTurn = -2;
		static long portLoadAtTurn = -2;
		static bool portSaveTriggered = false;
		static bool portLoadTriggered = false;
		if (portSaveAtTurn == -2)
		{
			const char* value = getenv("MC2_SAVE_AT_TURN");
			portSaveAtTurn = value && *value ? atol(value) : -1;
			value = getenv("MC2_LOAD_AT_TURN");
			portLoadAtTurn = value && *value ? atol(value) : -1;
		}
		if (!portSaveTriggered && portSaveAtTurn > 0 && turn >= portSaveAtTurn)
		{
			portSaveTriggered = true;
			saveInMissionSave = true;
			printf("[save-roundtrip] requesting save at turn %ld\n", turn);
		}
		if (!portLoadTriggered && portLoadAtTurn > 0 && turn >= portLoadAtTurn)
		{
			portLoadTriggered = true;
			loadInMissionSave = true;
			printf("[save-roundtrip] requesting load at turn %ld\n", turn);
		}

		// PORT: M7-B1 soak (2026-08-15). The gates above fire ONCE, and one clean
		// round trip proves nothing: the owner's heap corruption took a long
		// campaign run and surfaced at three DIFFERENT delete sites, each right
		// after a checkpoint reporting the heap intact. This repeats the same
		// Mission::save/Mission::load pair forever so an unattended headless run
		// can attempt hundreds of cycles.
		//
		// MC2_SAVE_LOAD_CYCLE=N -- save every N ticks, load N/2 ticks later.
		//
		// Scheduled on its OWN tick counter, not on `turn`: Mission::load restores
		// the saved turn, so a turn-based schedule would replay the same instant
		// forever and LOOK like progress while testing one cycle repeatedly.
		//
		// The cycle count is printed every cycle and flushed, because a run that
		// dies mid-soak is only comparable to another run if it says how many
		// cycles it survived. A NEGATIVE RESULT -- thousands of cycles, no
		// corruption -- is a real outcome: it would mean the interactive path
		// carries something this one does not, and the next probe belongs there.
		static long portSoakCycle = -2;
		static unsigned long portSoakTicks = 0;
		static unsigned long portSoakCount = 0;
		static int portSoakLoadOnly = -1;
		if (portSoakCycle == -2)
		{
			const char* soakValue = getenv("MC2_SAVE_LOAD_CYCLE");
			portSoakCycle = soakValue && *soakValue ? atol(soakValue) : -1;
			const char* loadOnly = getenv("MC2_SAVE_LOAD_LOAD_ONLY");
			portSoakLoadOnly = (loadOnly && *loadOnly && strcmp(loadOnly, "0") != 0) ? 1 : 0;
			if (portSoakCycle > 1 && portSoakLoadOnly)
				printf("[soak] armed LOAD-ONLY: reloading the save already on disk "
				       "every %ld ticks. Nothing is written, so a supplied save "
				       "survives the run.\n", portSoakCycle);
			else if (portSoakCycle > 1)
				printf("[soak] armed: save every %ld ticks, load %ld later\n",
				       portSoakCycle, portSoakCycle / 2);
		}
		if (portSoakCycle > 1)
		{
			portSoakTicks++;
			const unsigned long phase = portSoakTicks % (unsigned long)portSoakCycle;
			if (phase == 0 && !portSoakLoadOnly)
			{
				saveInMissionSave = true;
				portSoakCount++;
				printf("[soak] cycle %lu: save at tick %lu (turn %ld)\n",
				       portSoakCount, portSoakTicks, turn);
				fflush(stdout);
			}
			else if (phase == (unsigned long)(portSoakCycle / 2) ||
			         (portSoakLoadOnly && phase == 0))
			{
				if (portSoakLoadOnly) portSoakCount++;
				MC2_SoakIssueGuardOrder(portSoakCount);
				loadInMissionSave = true;
				printf("[soak] cycle %lu: load at tick %lu (turn %ld)\n",
				       portSoakCount, portSoakTicks, turn);
				fflush(stdout);
			}
		}

		memset(ObjectManager->moverLineOfSightTable, -1, ObjectManager->maxMovers*ObjectManager->maxMovers);

#ifdef LAB_ONLY
		MCTimeLOSCalc = 0;
		MCTimeAnimationCalc = 0;
#endif

		// Retail slow-motion. Inert: nothing has set forcedFrameRate since the
		// dev key chords were replaced by the panel. Left as retail wrote it.
		if (forcedFrameRate != -1.0f)
			frameLength /= forcedFrameRate;

		// PORT: the DevTools speed control. A MULTIPLIER, because dividing the
		// time step is what made the panel slow the game down when the owner
		// asked it to speed up. More simulated time per rendered frame = faster.
		//
		// Clamped exactly as MechCmd2.cpp clamps the unscaled value: a step much
		// beyond a quarter second destabilises movement and AI. At a low frame
		// rate this caps the speed-up short of the requested multiplier, which is
		// the right trade -- a stable 6x beats an unstable 10x.
		const float devScale = MC2_DevTimeScale();
		if (devScale != 1.0f)
		{
			const float beforeScale = frameLength;
			frameLength *= devScale;
			if (frameLength > 0.25f)
				frameLength = 0.25f;

			// Report what was ACTUALLY applied, including the clamp, every 200
			// ticks. frameLength is the quantity that drives game speed, so this
			// measures the fix rather than a proxy for it.
			static long scaleReport = 0;
			if ((scaleReport++ % 200) == 0)
			{
				fprintf(stderr,
					"[devtools] frameLength %.5f -> %.5f (x%.2f requested, x%.2f applied)\n",
					beforeScale, frameLength, devScale,
					beforeScale > 0.0f ? frameLength / beforeScale : 0.0f);
				fflush(stderr);
			}
		}

		MC2_DevToolsUpdate();

		if ((missionLineChanged + 50) < turn)
		{
			// The retail ctrl+alt+shift chords that used to live here are now rows
			// in the INSERT panel. See devtools.cpp.
		}
	
		//---------------------------------------
		// The game believes we will drop well below par for a frame or two.
		// put the game into slo-mo so we don't lose any frames of animation.
		dynamicFrameTiming = true;
			
		//---------------------------------------
		//Save the game in-Mission if requested
		if (saveInMissionSave)
		{
			save("data\\savegame\\testgame.ims");
			saveInMissionSave = false;
		}

		//--------------------------------------------------
		// Update length of time scenario has been running.
		if (!missionInterface->isPaused() || MPlayer )
		{
			//First Frame we just set LastTimeGetTime.
			// After that, it increments based on System Time.
			// NOT the crazy GameOS frameRate.
			DWORD currentTimeGetTime = timeGetTime();
			if (LastTimeGetTime != 0xffffffff)
			{
				float milliseconds = currentTimeGetTime - LastTimeGetTime;

				// M8-B69. THE SIMULATION CLOCK IS THE WALL CLOCK, even under
				// MC2_FIXED_FPS. frameLength is pinned there but scenarioTime is not,
				// so two runs of the same 2000 ticks reach different mission times --
				// measured, 4.3620 to 4.5880 across six runs -- and end with the world
				// in slightly different states. That is why the strict bar's long run
				// is not reproducible: nine runs at its own pinned config produced six
				// different texture-creation counts, 517 to 558, with the eviction
				// count flat at 389 for most of them -- the game DREW different things,
				// it did not evict differently.
				//
				// Fixed-FPS mode exists to make a run repeatable. A repeatable run
				// cannot read a clock nobody controls, so in that mode the simulation
				// advances by the same frameLength the renderer is pinned to. Live play
				// never sets the variable and is untouched.
				static int fixedStep = -1;
				if (fixedStep < 0)
				{
					const char* fps = getenv("MC2_FIXED_FPS");
					fixedStep = (fps && fps[0] && strcmp(fps, "0")) ? 1 : 0;
				}
				if (fixedStep)
					scenarioTime += frameLength;
				else
					scenarioTime += (milliseconds / 1000.0f);

				// PORT PROBE (M8-B69). The strict bar's 2000-tick run is
				// intermittently non-exact -- same binary, one run in three. The
				// candidate cause is right here: MC2_FIXED_FPS pins frameLength, but
				// scenarioTime is advanced from timeGetTime, the WALL CLOCK, so two
				// runs of the same 2000 ticks reach different mission times and
				// therefore different game state.
				//
				// This probe can come back NEGATIVE, which is the point: if three runs
				// report the same scenarioTime and the hashes still differ, the
				// hypothesis is dead and the cause is elsewhere. Opt-in, and printed
				// once at the end so it cannot itself perturb the run.
				{
					static long simTick = 0;
					const char* want = getenv("MC2_SIM_CENSUS");
					if ( want && want[0] && strcmp(want, "0") )
					{
						simTick++;
						// PORT INSTRUMENT (M8-B66): drive the capture experiment.
						{
							extern void MC2_CaptureProbeTick (void);
							MC2_CaptureProbeTick();
						}
						// PORT INSTRUMENT (M8-B70): the building census runs at mission
						// START, which is before Building::update has ever run -- so a
						// change that marks ground impassable during update is invisible
						// to it. MC2_CENSUS_AT_TICK takes the same dump later.
						{
							const char* at = getenv("MC2_CENSUS_AT_TICK");
							if ( at && at[0] && (simTick == atol(at)) )
							{
								extern void MC2_ObjectiveCensus (const char* when);
								MC2_ObjectiveCensus("late sample");
							}
						}
						if ( (simTick % 500) == 0 )
							fprintf(stderr, "[sim-census] tick=%ld turn=%ld scenarioTime=%.4f frameLength=%.5f\n",
								simTick, (long)turn, (double)scenarioTime, (double)frameLength);
					}
				}
			}
			LastTimeGetTime = currentTimeGetTime;

			soundSystem->clearIsPaused();
		}
		else
		{
			//Keep track of system time.  Just don't add it to scenarioTime!!
			DWORD currentTimeGetTime = timeGetTime();
			LastTimeGetTime = currentTimeGetTime;

			soundSystem->setIsPaused();
		}

		//--------------------------------------------------
		
		//------------------------------------------------------------------------
		// There is a TINYYYYYYYYYY chance this will never go if timeGetTime()
		// happens to return 0 (one millisecond every approx. 49 days). I can live
		// with that...

//		if (MPlayer && (MPlayer->startTime >= 0.0))
//			runningTime = (float)(gos_GetElapsedTime()) - MPlayer->startTime;
//		else if (MPlayer && (scenarioTime > 10.0))
//			Fatal(MissionStartTime, " runningTime is not working...why? ");

//		if (MPlayer)
//			actualTime = runningTime;
//		else
		//Try ALWAYS using SCENARIO Time.  This has been specially coded to be nummies!!
			actualTime = scenarioTime;

#if 0
		static bool tested = false;
		if ((scenarioTime > 15.0) && !tested) {
			tested = true;
			ABLFile* ablSaveFile;
			ABLi_saveEnvironment (ABLFile* ablFile) {

		}
#endif

		mcTextureManager->clearArrays();

		if (missionInterface)
			MC2_PHASE(MC2_PHASE_INTERFACE,
				ProfileTime(MCTimeInterfaceUpdate,missionInterface->update()));

		MC2_CameraGotoRoad(turn);
		MC2_PHASE(MC2_PHASE_CAMERA, ProfileTime(MCTimeCameraUpdate,eye->update()));
		// After eye->update(), or the camera's own goal-seeking undoes it.
		MC2_CameraPreset(turn);

		MC2_PHASE(MC2_PHASE_VTOL, missionInterface->updateVTol());

		MC2_PHASE(MC2_PHASE_TERRAIN, ProfileTime(MCTimeTerrainUpdate,land->update()));

		//ALWAYS update weather AFTER the camera.  May change the lights!
		if (useNonWeaponEffects)
			MC2_PHASE(MC2_PHASE_WEATHER,
				ProfileTime(MCTimeWeatherUpdate,weather->update()));		//Should the rain fall during a pause?

		MC2_PHASE(MC2_PHASE_WAYPOINTS, missionInterface->updateWaypoints());

#ifdef USE_PATH_COST_TABLE
		GlobalMoveMap[0]->resetPathCostTable();
#endif
		MC2_PHASE(MC2_PHASE_PATHMGR,
			ProfileTime(MCTimePathManagerUpdate,PathManager->update()));

		if (KillAmbientLight) {
	//		ambientRed<<16)+(ambientGreen<<8)+ambientBlue;
		}
		
		//-----------------------------------------------------------
		// Lastly, process the terrain geometry which loads textures
		// Must do this to keep from Locking during the updateRenders phase
		// Also reset the object flags because we recalc those during geometry!
		land->clearObjBlocksActive();
		land->clearObjVerticesActive();
		MC2_PHASE(MC2_PHASE_TERRAINTEX, land->terrainTextures->update());


		MC2_PHASE(MC2_PHASE_GEOMETRY, ProfileTime(MCTimeTerrainGeometry,land->geometry()));

		if ( missionInterface->isPaused() && !MPlayer )
			MC2_PHASE(MC2_PHASE_OBJECTS,
				ObjectManager->updateAppearancesOnly( true, true, true ));
		else
			MC2_PHASE(MC2_PHASE_OBJECTS, ObjectManager->update(true, true, true));

		MC2_PHASE(MC2_PHASE_CRATERS, ProfileTime(MCTimeCraterUpdate,craterManager->update()));

		//Do not UPDATE the textures during a pause.
		//This uncaches things which only objectManager->update can cache back in!!!!!
		if ( !missionInterface->isPaused() || MPlayer )
			MC2_PHASE(MC2_PHASE_TXMMGR,
				ProfileTime(MCTimeTXMManagerUpdate,mcTextureManager->update()));

		//--------------------------------------
		// update sensor and contact managers...
		if (MC2_SensorGateCensus && !MC2_SensorGateAnnounced)
		{
			MC2_SensorGateAnnounced = true;
			printf("[sensor-gate] census active: useSensors=%d isPaused=%d MPlayer=%d SensorManager=%d\n",
			       useSensors ? 1 : 0,
			       (missionInterface && missionInterface->isPaused()) ? 1 : 0,
			       MPlayer ? 1 : 0,
			       SensorManager ? 1 : 0);
			fflush(stdout);
		}

		if (useSensors && ( !missionInterface->isPaused() || MPlayer ) )
		{
			if (MC2_SensorGateCensus)
			{
				++MC2_SensorUpdateCalls;
				if ((MC2_SensorUpdateCalls % 500) == 0)
				{
					printf("[sensor-gate] SensorManager->update() calls=%ld skips=%ld\n",
					       MC2_SensorUpdateCalls, MC2_SensorGateSkips);
					fflush(stdout);
				}
			}
			MC2_PHASE(MC2_PHASE_SENSORS,
				ProfileTime(MCTimeSensorUpdate, SensorManager->update()));
		}
		else if (MC2_SensorGateCensus)
		{
			++MC2_SensorGateSkips;
			if ((MC2_SensorGateSkips % 500) == 0)
			{
				printf("[sensor-gate] SKIPPED: useSensors=%d isPaused=%d skips=%ld\n",
				       useSensors ? 1 : 0,
				       (missionInterface && missionInterface->isPaused()) ? 1 : 0,
				       MC2_SensorGateSkips);
				fflush(stdout);
			}
		}

		if (useCollisions && ( !missionInterface->isPaused() || MPlayer ) )
			MC2_PHASE(MC2_PHASE_COLLISIONS,
				ProfileTime(MCTimeCollisionUpdate,ObjectManager->updateCollisions()));

		if (missionBrain)
		{
			if ( !missionInterface->isPaused() || MPlayer )
			{
				MC2_PHASE(MC2_PHASE_ABL,
					ProfileTime(MCTimeMissionScript,missionBrain->execute()));
				long missionResult = missionBrain->getInteger();
				if (missionResult == 9999)
					return(terminationResult = 9999);
				if (!MPlayer)
					terminationResult = missionResult;
			}
		}

		//----------------------------------------------------
		// Check is all player forces dead/disabled.
		if (!MPlayer && !terminationCounterStarted)
		{
			bool playerForceAllDead = true;
			long i;
			for (i=0;i<ObjectManager->getNumMovers();i++)
			{
				MoverPtr mover = ObjectManager->getMover(i);
				if (mover->getCommanderId() == Commander::home->getId())
					if (!mover->isDisabled() && !mover->isDestroyed() && mover->isOnGUI()) {
						playerForceAllDead = false;
						break;
					}
			}

			if (playerForceAllDead)
			{
				terminationResult = mis_PLAYER_LOST_BIG;
				terminationCounterStarted = true;
				missionTerminationTime = actualTime + 5.0/*seconds*/;
			}
		}

		if (terminationCounterStarted) 
		{
			if (missionTerminationTime <= actualTime) {
				if (ControlGui::instance->resultsDone()) {
					/* if the gui is finished rendering objective results then end it */
					scenarioResult = terminationResult;

					if ( !scenarioResult )
					{
						// mission brain frequently trashes the termination result
						if (!MPlayer) {
							int status = Team::home->objectives.Status();
							if (OS_SUCCESSFUL == status) 
								scenarioResult = mis_PLAYER_WIN_BIG;
							else
								scenarioResult = mis_PLAYER_LOST_BIG;
						}
					}

					// need to reset the termination result
				}
				else if (!MPlayer) {
					/* if all the player's dudes are dead then end it */
					bool playerForceAllDead = true;
					long i;
					for (i=0;i<ObjectManager->getNumMovers();i++)
					{
						MoverPtr mover = ObjectManager->getMover(i);
						if (mover->getCommanderId() == Commander::home->getId())
							if (!mover->isDisabled() && !mover->isDestroyed() && mover->isOnGUI()) {
								playerForceAllDead = false;
								break;
							}
					}

					if (playerForceAllDead)
					{
						terminationResult = mis_PLAYER_LOST_BIG;
						terminationCounterStarted = true;
						missionTerminationTime = actualTime + 5.0/*seconds*/;
					}
				}
			}
		} 
		else 
		{
			//As long as the ABL script returns -1, we won't do this.  NEEDED for Tutorials
			if (terminationResult != -1)
			{
				if (MPlayer) 
				{
					if (MPlayer->calcMissionStatus()) 
					{
						terminationCounterStarted = true;
						missionTerminationTime = actualTime + 5.0/*seconds*/;
						if (MPlayer->playerInfo[MPlayer->commanderID].winner)
							terminationResult = mis_PLAYER_WIN_BIG;
						else
							terminationResult = mis_PLAYER_LOST_BIG;

					}
				}
				else if (1 <= Team::home->objectives.Count()) 
				{
					int status = Team::home->objectives.Status();
					if (0 < Team::home->numPrimaryObjectives) 
					{
						if (OS_UNDETERMINED != status) 
						{
							if (true)   // PORT: never-ending story removed 2026-08-21
							{
								if (OS_SUCCESSFUL == status) 
								{
									terminationResult = mis_PLAYER_WIN_BIG;
									terminationCounterStarted = true;
									missionTerminationTime = actualTime + 5.0/*seconds*/;
								} 
								else 
								{
									terminationResult = mis_PLAYER_LOST_BIG;
									terminationCounterStarted = true;
									missionTerminationTime = actualTime + 5.0/*seconds*/;
								}
							}
						}
					}
				}
			}
		}

		if (!MPlayer && !terminationCounterStarted)
		{
			if ((m_timeLimit != -1.0f) && (actualTime > m_timeLimit))
			{
				terminationResult = mis_PLAYER_LOST_BIG;
				terminationCounterStarted = true;
				missionTerminationTime = actualTime + 5.0/*seconds*/;
			}
		}

		if (userInput->getKeyDown(KEY_F) && !userInput->ctrl() && userInput->alt() && !userInput->shift())
		{
			showFrameRate ^= true;
			missionLineChanged = turn;
		}

		if (MC2_DevToolsEnabled())
		{
			// scenarioResult is a Mission member, so the panel cannot set it. It
			// hands back a consume-once request and this applies it in the scope
			// that owns it.
			const MC2DevRequest devRequest = MC2_DevToolsTakeRequest();
			if (devRequest == MC2_DEV_REQUEST_WIN)
				scenarioResult = mis_PLAYER_WIN_BIG;
			else if (devRequest == MC2_DEV_REQUEST_LOSE)
				scenarioResult = mis_PLAYER_LOST_BIG;
		}


		//FORCE these to play to thwart any ABL bugs.
		// OPPOSITE for a reason!!!!!!!!
		// Duane decided to switch the order for our conveniance.  Thanks!!
		if ((terminationResult) && (terminationResult != -1) && (terminationResult < mis_PLAYER_DRAW))
		{
			soundSystem->playDigitalMusic(WIN_TUNE_0);
		}
		else if (terminationResult > mis_PLAYER_DRAW)
		{
			soundSystem->playDigitalMusic(LOSE_TUNE_0);
		}

		if (showFrameRate)
		{
			char text[1024];
			sprintf(text,"FrameRate: %f",1.0f/frameLength);
		
			DWORD width, height;
			Stuff::Vector4D moveHere;
			moveHere.x = 0.0f;
			moveHere.y = 0.0f;
		
			gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
			gos_TextStringLength(&width,&height,text);
		
			moveHere.z = width;
			moveHere.w = height;
		
			globalFloatHelp->setFloatHelp(text,moveHere,SD_GREEN,XP_BLACK,1.0f,true,false,false,false);
		}

#ifdef LAB_ONLY
		MCTimeMissionTotal 		= MCTimeTerrainUpdate +
									MCTimeCameraUpdate +
									MCTimeWeatherUpdate + 
									MCTimePathManagerUpdate +
									MCTimeRunBrainUpdate +
									MCTimePath1Update + 
									MCTimePath2Update + 
									MCTimePath3Update + 
									MCTimePath4Update + 
									MCTimePath5Update + 
									MCTimeCalcPath1Update +
									MCTimeCalcPath2Update +
									MCTimeCalcPath3Update +
									MCTimeCalcPath4Update +
									MCTimeCalcPath5Update +
									MCTimeCalcGoal1Update + 
									MCTimeCalcGoal2Update + 
									MCTimeCalcGoal3Update + 
									MCTimeCalcGoal4Update + 
									MCTimeCalcGoal5Update + 
									MCTimeCalcGoal6Update + 
									MCTimeTerrainGeometry +
									MCTimeInterfaceUpdate +
									MCTimeCraterUpdate +
									MCTimeTXMManagerUpdate +
									MCTimeSensorUpdate +
									MCTimeLOSUpdate +
									MCTimeCollisionUpdate +
									MCTimeMissionScript+ 
									MCTimeTerrainObjectsUpdate +
									MCTimeMechsUpdate +
									MCTimeVehiclesUpdate +
									MCTimeTurretsUpdate +
									MCTimeAllElseUpdate;
#endif
	}

	return scenarioResult;
}

//----------------------------------------------------------------------------------

long Mission::getStatus (void) {

	if (terminationCounterStarted)
		return(terminationResult);
	return(mis_PLAYING);
}

//----------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PORT (M8-B18): render-split accumulators. See mc2_render_split.h for why this
// accumulates instead of printing, and for what the phases mean.
//-----------------------------------------------------------------------------
#include "mc2_render_split.h"

static const char* const MC2_RenderPhaseNames[MC2_RP_COUNT] =
{
	"state setup", "mission->render", "logistics", "userInput", "debugWins",
	"  eye->render", "  devTools", "  missionInterface",
	"    sky", "    land(build)", "    craters", "    objects(build)",
	"    water", "    shadows", "    compass", "    renderLists(SUBMIT)"
};

double MC2_RenderPhaseTotal[MC2_RP_COUNT] = {0.0};
long   MC2_RenderPhaseFrames = 0;

static bool MC2_RenderSplitOn = false, MC2_RenderSplitChecked = false;

static void MC2_RenderSplitSummary (void)
{
	if (!MC2_RenderSplitOn || MC2_RenderPhaseFrames <= 0) return;
	const double frames = (double)MC2_RenderPhaseFrames;
	double accounted = 0.0;
	for (long i = 0; i < MC2_RP_TOPLEVEL_COUNT; i++)
		accounted += MC2_RenderPhaseTotal[i];
	printf("[render-split] over %ld frames, mean ms per frame "
	       "(indented rows are INSIDE mission->render and are not re-counted):\n",
	       MC2_RenderPhaseFrames);
	for (long i = 0; i < MC2_RP_COUNT; i++)
		printf("[render-split]   %-20s %7.3f ms  (%5.1f%%)\n",
		       MC2_RenderPhaseNames[i],
		       MC2_RenderPhaseTotal[i] / frames,
		       accounted > 0.0 ? 100.0 * MC2_RenderPhaseTotal[i] / accounted : 0.0);
	printf("[render-split]   %-20s %7.3f ms  (top-level sum)\n",
	       "ACCOUNTED", accounted / frames);
	fflush(stdout);
}

bool MC2_RenderSplitArmed (void)
{
	if (!MC2_RenderSplitChecked)
	{
		MC2_RenderSplitChecked = true;
		const char* e = getenv("MC2_RENDER_SPLIT");
		MC2_RenderSplitOn = e && *e && strcmp(e, "0") != 0;
		if (MC2_RenderSplitOn) atexit(MC2_RenderSplitSummary);
	}
	return MC2_RenderSplitOn;
}

void MC2_RenderPhaseAdd (long id, double ms)
{
	if (id >= 0 && id < MC2_RP_COUNT) MC2_RenderPhaseTotal[id] += ms;
}

void MC2_RenderPhaseFrame (void)
{
	if (MC2_RenderSplitArmed()) MC2_RenderPhaseFrames++;
}

long Mission::render (void)
{
	if (active)
	{
		unsigned char tempAmbientLight[3];
		if (KillAmbientLight) {
			tempAmbientLight[0] = eye->ambientRed;
			tempAmbientLight[1] = eye->ambientGreen;
			tempAmbientLight[2] = eye->ambientBlue;
			eye->ambientRed = 0xFF;
			eye->ambientGreen = 0xFF;
			eye->ambientBlue = 0xFF;
		}
		MC2_RENDER_PHASE(MC2_RP_EYE, eye->render());
		GameMap->clearCellDebugs(0);
	
		//-----------------------------------------------------
		// FOG time.  Set Render state to FOG on!
		DWORD fogColor = eye->fogColor;
		if (useFog)
		{
			gos_SetRenderState( gos_State_Fog, (intptr_t)&fogColor);
		}
		else
		{
			gos_SetRenderState( gos_State_Fog, 0);
		}
		
		gos_SetRenderState( gos_State_Fog, 0);
		
		FloatHelp::renderAll();
		MC2_RENDER_PHASE(MC2_RP_DEVTOOLS, MC2_DevToolsRender());
	
		currentFloatHelp = 0;
	
		if (missionInterface)
			MC2_RENDER_PHASE(MC2_RP_INTERFACE, missionInterface->render());

		if (KillAmbientLight) {
			eye->ambientRed = tempAmbientLight[0];
			eye->ambientGreen = tempAmbientLight[1];
			eye->ambientBlue = tempAmbientLight[2];
		}
		
		//reset the TGL RAM pools.
		colorPool->reset();
		{ extern void MC2_PoolFrameReset(void); MC2_PoolFrameReset(); }
		vertexPool->reset();
		facePool->reset();
		shadowPool->reset();
		trianglePool->reset();
	}
		
	return scenarioResult;
}

//----------------------------------------------------------------------------------
float applyDifficultySkill (float chance, bool isPlayer)
{
	if (isPlayer)
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium
			case 2:			//Hard
			case 3:			//Really Hard
				chance *= (float(globalPlayerSkills[GameDifficulty]) / 100.0);
				return(chance);
				break;

			default:
				return (chance);
				break;
		}
	}
	else
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium
			case 2:			//Hard
			case 3:			//Really Hard
				chance *= (float(globalEnemySkills[GameDifficulty]) / 100.0);
				return(chance);
				break;

			default:
				return(chance);
				break;
		}
	}
}

//----------------------------------------------------------------------------------
float applyDifficultyWeapon (float dmg, bool isPlayer)
{
	if (isPlayer)
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium -- NO CHANGE!
			case 2:			//Hard
			case 3:			//Very Hard
				dmg *= (float(globalPlayerWeapons[GameDifficulty]) / 100.0);
				break;
				
			default:
				break;
		}
	}
	else
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium -- NO CHANGE!
			case 2:			//Hard
			case 3:			//Very Hard
				dmg *= (float(globalEnemyWeapons[GameDifficulty]) / 100.0);
				break;
				
			default:
				break;
		}
	}

	//------------------------------------------------------------------
	// Must keep damage an integral part of .25 or bad things happen
	// In theory only needed for Multiplayer but better safe then...
	if (((float)((long)(dmg / 0.25)) * 0.25) != dmg)
	{
		dmg = ((float)((long)(dmg / 0.25)) * 0.25);
	}

	if (dmg < 0.0)
		dmg = 0.0;

	if (dmg > 255.0)
		dmg = 255.0;
		
	return dmg;
}

//----------------------------------------------------------------------------------
void InitDifficultySettings (FitIniFile *gameSystemFile)
{
	long result = gameSystemFile->seekBlock("DifficultySettings");
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLongArray("PlayerSkills", globalPlayerSkills,4);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLongArray("EnemySkills",globalEnemySkills,4);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLongArray("PlayerWeapons",globalPlayerWeapons,4);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLongArray("EnemyWeapons",globalEnemyWeapons,4);
	gosASSERT(result == NO_ERR);
}	

//--------------------------------------------------
// Game System Constants -- Definitions here.
float maxVisualRange = 0.0;
unsigned long MaxTreeLOSCellBlock = 0;
float MaxVisualRadius = 0.0;
float fireVisualRange = 0.0;
extern float WeaponRange[];						//MOVER
extern float DefaultAttackRange;				//MOVER
float baseSensorRange = 0.0;
extern long visualRangeTable[];

extern unsigned char godMode;		//Can I simply see everything, enemy and friendly?
unsigned char revealTacMap = 0;		//Is Tacmap always revealed?
unsigned char footPrints = 0;		//Do we use/draw footprints?
bool CantTouchThis = false;

long tonnageDivisor = 1.0;			//Amount we divide tons under drop weight by to calc resource bonus
long resourcesPerTonDivided = 0;	//Tons per above divided number

float MineDamage = 0.0;			//Damage mine does per tile
float MineSplashDamage = 0.0;	//Damage mine explosion does to all in tile.
float MineSplashRange = 0.0;	//Range of Splash Damage
long MineExplosion = -1;		//Object ID of mine explosion
long MineLayThrottle = 0;		//Throttle setting of mine Layer
long MineSweepThrottle = 0;		//Throttle setting of mine sweeper
float StrikeWaitTime = 0.0f;	//Time between aerospace Spotter strike calls
float StrikeTimeToImpact = 0.0f;//Time it takes air strike to come in.
float MineWaitTime = 0.0;		//Time Mine Layer must sit in Tile before mine is laid.

long totalSmokeSpheres = 0;			//Maximum number of smoke spheres in world.
long totalSmokeShapeSize = 0;		//Amount we allocate to smoke heap for spheres/textures.

long maxFiresBurning = 0;			//Maximum number of fires allowed in world.
float maxFireBurnTime = 0.0;		//Maximum time a fire can be burning before it dies out.

bool force64MB = TRUE;				//Do we have between 64 and 32 Mb of memory?
bool force32Mb = FALSE;				//Do we have 32Mb of memory only?

float InfluenceRadius = -1.0;		//Capture Radius
float InfluenceTime = 0.0;			//Time inside to Capture

//----------------------------------------------------------------------------------

void Mission::createPartObject (long partIndex, MoverPtr mover) {

	//-----------------------------------
	// All parts are movers in this game!
	
	//------------------------------------------------------------------
	// Create the object -- If object is Not DESTROYED Already OR
	// it doesn't exist yet.  (parts[i].object = NULL
	//
	// This could NEVER have worked.  Destroyeds still need to be created.  Otherwise,
	// why is there code inside this IF which checks if destroyed?  I blame Glenn's Crazy Bracing Style!
	// Create the part just like always, just start it disabled.
	// 
	// If the mech is not on OUR team, you can recover it during the mission.
	// If the mech IS on our team, it starts shutdown and NOT added to Heidi's interface.
	// When we get close enough to it, it adds itself to Heidi's world and we can command it!
	if (parts[partIndex].objectWID == 0) 
	{
		ObjectTypePtr objType = ObjectManager->getObjectType(parts[partIndex].objNumber);
		Assert(objType != NULL, partIndex, " Mission.createPartObject: unable to get objType ");
		parts[partIndex].objectWID = mover->getWatchID();
		mover->init(true, objType);
		mover->setAwake(parts[partIndex].active ? true : false);
		mover->setHandle(ObjectManager->getHandle(mover));

		//----------------------------------------------
		// Load the profile data into the game object...
		// This is MONDO DIFFERENT NOW.
		// Profiles are in the mech's CSV file.  You should just init with mover with
		// the variant number and the rest is done in the mover itself.
		//
		// Heidi, this is what needs to change in the mission.fit files!
		// -fs 12/7/99		A date which will live in Infamy.
		// Must also load Ground Vehicles the OLDen WAY until further notice.
		switch (mover->getObjectClass()) 
		{
			case BATTLEMECH: 
				mover->init(parts[partIndex].variantNum);		
				break;

			case GROUNDVEHICLE:
			case ELEMENTAL:
			{
				if (ObjectTypeManager::objectFile->seekPacket(parts[partIndex].objNumber) == NO_ERR) 
				{
					//--------------------------------------------------------
					// All new code here.  This will ask the objectType it is
					// loading what kind of objectType it is and create it
					// based on that instead of objTypeNum.
					FitIniFile profileFile;
					long result = profileFile.open(ObjectTypeManager::objectFile,ObjectTypeManager::objectFile->getPacketSize());
					if (result != NO_ERR)
						STOP((" Mission.createPartObject: can't open file "));

					if (result == NO_ERR) 
					{
						//-------------------------------------------
						result = mover->init(&profileFile);
						if (result != NO_ERR)
							STOP((" Mission.createPartObject: bad Profile File "));
					}
					
					profileFile.close();
				}
			}
			break;
		}

		mover->setPilotHandle(parts[partIndex].pilot);
		mover->setTeamId(parts[partIndex].teamId, true);
		mover->setSquadId(parts[partIndex].squadId);
		mover->calcWeaponEffectiveness(TRUE);
		mover->calcWeaponEffectiveness(FALSE);
		
		if (parts[partIndex].controlType == CONTROL_PLAYER)
			mover->setControl(CONTROL_PLAYER);
		else if (MPlayer)
			mover->setControl(MPlayer->isServer() ? CONTROL_AI : CONTROL_NET);
		else
			mover->setControl(CONTROL_AI);

		mover->setPosition(parts[partIndex].position);
		mover->setLastValidPosition(parts[partIndex].position);
		mover->setRotation(parts[partIndex].rotation);
		mover->setCommanderId(parts[partIndex].commanderID);

		switch (mover->getObjectClass()) {
			case BATTLEMECH: {
				((BattleMechPtr)mover)->captureable = parts[partIndex].captureable;

				//-----------------------------------------
				// Make sure the pilot paints his Mech!
				// Use editor colors UNLESS this is on Commander::home
				AppearancePtr myActor = mover->getAppearance();

				if (MPlayer)
					myActor->resetPaintScheme( MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											   MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											   MPlayer->colors[MPlayer->playerInfo[mover->commanderId].baseColor[BASECOLOR_TEAM]]);
				else {
					if (mover->getCommanderId() == Commander::home->getId())
						myActor->resetPaintScheme(prefs.highlightColor, prefs.highlightColor, prefs.baseColor);
					else
						myActor->resetPaintScheme(parts[partIndex].highlightColor1,
												parts[partIndex].highlightColor2,
												parts[partIndex].baseColor);
					//myActor->setGesture(parts[partIndex].gestureId);  DOn't do this unless you want the mechs to start shutdown and power up!
				}
				}
				break;
			case GROUNDVEHICLE: {
				AppearancePtr myActor = mover->getAppearance();
				if (MPlayer)
					myActor->resetPaintScheme(MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											  MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											  MPlayer->colors[MPlayer->playerInfo[mover->commanderId].baseColor[BASECOLOR_TEAM]]);
				else {
					if (mover->getCommanderId() == Commander::home->getId())
						myActor->resetPaintScheme(prefs.highlightColor, prefs.highlightColor, prefs.baseColor);
					else
						myActor->resetPaintScheme(parts[partIndex].highlightColor1,
												  parts[partIndex].highlightColor2,
												  parts[partIndex].baseColor);
					//myActor->setGesture(parts[partIndex].gestureId);  DOn't do this unless you want the mechs to start shutdown and power up!
				}
				}
				break;
			case ELEMENTAL:
				break;
		}

		//-----------------------------------------------------------------
		// Set object Unique ID to be Part Number
		//parts[i].object->setIdNumber(i);

		if (MPlayer) {
			MPlayer->addToMoverRoster((MoverPtr)ObjectManager->getByWatchID(parts[partIndex].objectWID));
			MPlayer->addToPlayerMoverRoster(parts[partIndex].commanderID, (MoverPtr)ObjectManager->getByWatchID(parts[partIndex].objectWID));
			if (parts[partIndex].commanderID == MPlayer->commanderID)
				MPlayer->addToLocalMovers((MoverPtr)ObjectManager->getByWatchID(parts[partIndex].objectWID));
		}

		if (parts[partIndex].exists) 
		{
			mover->setExists(true);
		}
		else
			mover->setExists(false);

		if (parts[partIndex].destroyed) 
		{
			if (mover->getCommanderId() == Commander::home->getId())
			{
				mover->setStatus(OBJECT_STATUS_SHUTDOWN);
				mover->startDisabled = false;
				mover->setOnGUI(false);		//Even for non-player Team Parts.  It only FALSE if we link up with it later!!!!!
			}
			else
			{
				mover->setStatus(OBJECT_STATUS_DISABLED);
				mover->disable( UNDETERMINED_DEATH );
				mover->startDisabled = true;
				if (mover->sensorSystem)
					mover->sensorSystem->disable();
				mover->setOnGUI(true);		//Even for non-player Team Parts.  It only FALSE if we link up with it later!!!!!
			}
		}
		else
		{
			mover->setOnGUI(true);		//Even for non-player Team Parts.  It only FALSE if we link up with it later!!!!!
		}
	}
}

//---------------------------------------------------------------------------
long Mission::addMover (MoverInitData* moverSpec) {
	
	//--------------------------------------
	// Load the mechwarrior into the mech...
	MechWarriorPtr pilot = MechWarrior::newWarrior();
	if (!pilot)
		STOP(("Too many pilots in this mission!"));

	FullPathFileName pilotFullFileName;
	pilotFullFileName.init(warriorPath, moverSpec->pilotFileName, ".fit");
			
	FitIniFile* pilotFile = new FitIniFile;
	gosASSERT(pilotFile != NULL);
		
	long result = pilotFile->open(pilotFullFileName);
	gosASSERT(result == NO_ERR);
	result = pilot->init(pilotFile);
	gosASSERT(result == NO_ERR);

	pilotFile->close();
	delete pilotFile;
	pilotFile = NULL;

	//Copy logistics data to pilot AFTER loading old data.
	// ONLY if we overrode the data in logistics!!
	if (moverSpec->overrideLoadedPilot)
	{
		pilot->skills[MWS_GUNNERY] = pilot->skillRank[MWS_GUNNERY] = moverSpec->gunnerySkill;
		pilot->skills[MWS_PILOTING] = pilot->skillRank[MWS_PILOTING] = moverSpec->pilotingSkill;
		memcpy(pilot->specialtySkills,moverSpec->specialtySkills,sizeof(bool) * NUM_SPECIALTY_SKILLS);
		pilot->calcRank();
	}

	//*********************
	// NOTE: Need to send packet to other players in MP with new pilot and
	// mover data!
	//*********************

	long numErrors, numLinesProcessed;
	FullPathFileName brainFullFileName;
	if (MPlayer) {
		pilot->setBrainName("pbrain");
		brainFullFileName.init(warriorPath, "pbrain", ".abl");
		}
	else {
		pilot->setBrainName(moverSpec->brainFileName);
		brainFullFileName.init(warriorPath, moverSpec->brainFileName, ".abl");
	}
		
	long moduleHandle = ABLi_preProcess(brainFullFileName, &numErrors, &numLinesProcessed);
	gosASSERT(moduleHandle >= 0);
	pilot->setBrain(moduleHandle);

/*
typedef struct _MoverInitData {
	char			pilotFileName[50];
	char			brainFileName[50];
	char			profileName[50];
	long			objNumber;
	unsigned long	controlType;
	unsigned long	controlDataType;
	unsigned long	variant;
	float			position[2];
	long			rotation;
	char			teamID;
	char			commanderID;
	long			paintScheme;
	bool			active;
	bool			exists;
	char			icon;
	bool			capturable;
} MoverInitData;
*/
			//parts[i].gestureId = 2; // this has never changed
	ObjectTypePtr objType = ObjectManager->loadObjectType(moverSpec->objNumber);
	if (!objType)
		objType = ObjectManager->getObjectType(moverSpec->objNumber);
	// PORT: both lookups failing skips the entire block below -- no mover, no
	// roster entry, no unit bar icon, and no message. Same silent outcome as a
	// full mech pool, different cause, so name which one happened.
	if (!objType)
	{
		printf("[mission] NO OBJECT TYPE for objNumber=%ld -- mover NOT created\n",
		       (long)moverSpec->objNumber);
		fflush(stdout);
	}
	if (objType) {
		MoverPtr mover = NULL;
		switch (objType->getObjectTypeClass()) {
			case BATTLEMECH_TYPE:
				mover = (MoverPtr)ObjectManager->newMech();
				break;
			case VEHICLE_TYPE:
				mover = (MoverPtr)ObjectManager->newVehicle();
				break;
		}

		if (!mover)
		{
			printf("[mission] MOVER ALLOCATION FAILED for objNumber=%ld class=%d\n",
			       (long)moverSpec->objNumber, (int)objType->getObjectTypeClass());
			fflush(stdout);
		}
		if (mover) 
		{
			mover->init(true, objType);
			mover->setAwake(moverSpec->active);
			mover->setHandle(ObjectManager->getHandle(mover));

			//----------------------------------------------
			// Load the profile data into the game object...
			// This is MONDO DIFFERENT NOW.
			// Profiles are in the mech's CSV file.  You should just init with mover with
			// the variant number and the rest is done in the mover itself.
			//
			// Heidi, this is what needs to change in the mission.fit files!
			// -fs 12/7/99		A date which will live in Infamy.
			// Must also load Ground Vehicles the OLDen WAY until further notice.
			switch (mover->getObjectClass()) {
				case BATTLEMECH: 
					mover->init(moverSpec->variant);		
					break;
				case GROUNDVEHICLE:
				case ELEMENTAL:	
				{
					if (ObjectTypeManager::objectFile->seekPacket(moverSpec->objNumber) == NO_ERR) 
					{
						//--------------------------------------------------------
						// All new code here.  This will ask the objectType it is
						// loading what kind of objectType it is and create it
						// based on that instead of objTypeNum.
						FitIniFile profileFile;
						long result = profileFile.open(ObjectTypeManager::objectFile,ObjectTypeManager::objectFile->getPacketSize());
						if (result != NO_ERR)
							STOP((" Mission.createPartObject: can't open file "));
	
						if (result == NO_ERR) 
						{
							//-------------------------------------------
							result = mover->init(&profileFile);
							if (result != NO_ERR)
								STOP((" Mission.createPartObject: bad Profile File "));
						}
						
						profileFile.close();
					}
				}
				break;
			}

			mover->setPilotHandle(pilot->getIndex());
			mover->setTeamId(moverSpec->teamID, true);
			mover->calcWeaponEffectiveness(true);
			mover->calcWeaponEffectiveness(false);
		
			if (moverSpec->controlType == CONTROL_PLAYER)
				mover->setControl(CONTROL_PLAYER);
			else if (MPlayer)
				mover->setControl(MPlayer->isServer() ? CONTROL_AI : CONTROL_NET);
			else
				mover->setControl(CONTROL_AI);

			mover->setPosition(moverSpec->position);
			mover->setLastValidPosition(moverSpec->position);
			mover->setRotation(moverSpec->rotation);
			mover->setCommanderId(moverSpec->commanderID);

			switch (mover->getObjectClass()) {
				case BATTLEMECH: {
					((BattleMechPtr)mover)->captureable = moverSpec->capturable;
					//-----------------------------------------
					// Make sure the pilot paints his Mech!!			
					AppearancePtr myActor = mover->getAppearance();
					myActor->resetPaintScheme(moverSpec->highlightColor1,
											moverSpec->highlightColor2,
											moverSpec->baseColor);
					//myActor->setGesture(moverSpec->gestureID);  Don't do this unless you want things to start shutdown and power up!
					}
					break;
				case GROUNDVEHICLE:	{
					AppearancePtr myActor = mover->getAppearance();
					myActor->resetPaintScheme(moverSpec->highlightColor1,
											moverSpec->highlightColor2,
											moverSpec->baseColor);
					}
					break;
				case ELEMENTAL:
					break;
			}

			//-----------------------------------------------------------------
			// Set object Unique ID to be Part Number
			//parts[i].object->setIdNumber(i);

			if (MPlayer) {
				MPlayer->addToMoverRoster(mover);
				MPlayer->addToPlayerMoverRoster(moverSpec->commanderID, mover);
				if (moverSpec->commanderID == MPlayer->commanderID)
					MPlayer->addToLocalMovers(mover);
			}

			if (moverSpec->exists)
				mover->setExists(true);
			else
				mover->setExists(false);

			//---------------------------------------------------------------------------------
			// If we're not playing multiplayer, make sure all home commander movers have their
			// localMoverId set to 0, so the iface can at least check if a mover is player
			// controlled...
			if (!MPlayer && (Commander::home->getId() == moverSpec->commanderID))
				mover->setLocalMoverId(0);

			ObjectManager->modifyMoverLists(mover, MOVERLIST_ADD);
			Team::teams[moverSpec->teamID]->addToRoster(mover);
			missionInterface->addMover(mover);

			long i;
			for (i = 0; i < ObjectManager->getNumMovers(); i++) {
				MoverPtr curMover = ObjectManager->getMover(i);
				if (!curMover->isDisabled()) {
					if (curMover->sensorSystem)
						curMover->sensorSystem->scanMover(mover);
					if ( curMover->getPilot() )
						curMover->getPilot()->triggerAlarm(PILOT_ALARM_NEW_MOVER, mover->getWatchID());
				}
			}
	
			if (moverSpec->numComponents > 0) {
				if (mover->getObjectClass() != BATTLEMECH)
					STOP(("LogisticsMech was not a MECH!!"));
				((BattleMechPtr)mover)->resetComponents(moverSpec->numComponents, moverSpec->components);
			}	
			return(mover->getHandle());
		}
	}
	else
		return -1;

	return(0);
}

//---------------------------------------------------------------------------

long Mission::addMover (MoverInitData* moveSpec, LogisticsMech* mechData)
{
	//OK, whole new ballgame here.
	// First, convert Heidi data to MoverInitData.
	// Then, create Mech using this data.
	// Then, Replace components to match logisticsMech Components.
	// Then, it should be good to go.
  
	MoverInitData mData;
	memset(&mData,0,sizeof(MoverInitData));
	
	strncpy(mData.pilotFileName,mechData->getPilot()->getFileName(),49);
	strcpy(mData.brainFileName,"pbrain");
	strncpy(mData.csvFileName,mechData->getFileName(),50);
	
	mData.rosterIndex = 255;
	mData.objNumber = mechData->getFitID();
	mData.controlType = moveSpec->controlType;
	mData.controlDataType = moveSpec->controlDataType;
	mData.variant = moveSpec->variant;
	mData.position.x = moveSpec->position.x;
	mData.position.y = moveSpec->position.y; 
	mData.position.z = moveSpec->position.z; 
	mData.rotation = moveSpec->rotation;
	mData.teamID = moveSpec->teamID;
	mData.commanderID = moveSpec->commanderID;
	mData.baseColor = moveSpec->baseColor;
	mData.highlightColor1 = moveSpec->highlightColor1;
	mData.highlightColor2 = moveSpec->highlightColor2;
	mData.gestureID = 2;
	mData.active = true;
	mData.exists = true;
	mData.icon = 0;
	mData.capturable = false;
	mData.overrideLoadedPilot = moveSpec->overrideLoadedPilot;
	mData.gunnerySkill = moveSpec->gunnerySkill;
	mData.pilotingSkill = moveSpec->pilotingSkill;
	memcpy(mData.specialtySkills,moveSpec->specialtySkills,sizeof(bool) * NUM_SPECIALTY_SKILLS);

	long moverHandle = addMover(&mData);

	//Now take the moverHandle and change the component data to match Heidi's passed logisticsMech.
	MoverPtr mMech = (MoverPtr)ObjectManager->get(moverHandle);
	if (mMech->getObjectClass() != BATTLEMECH)
		STOP(("LogisticsMech was not a MECH!!"));

	strcpy( ((BattleMech*)mMech)->variantName, mechData->getName() );
		
	long totalComponents = mechData->getComponentCount();

	long *componentList = NULL;
	
	if ( totalComponents )
	{
		componentList = (long *)systemHeap->Malloc(sizeof(long) * totalComponents);
	
		long otherCount = totalComponents;
		mechData->getComponents(otherCount, componentList);

		if (otherCount != totalComponents)
			STOP(("Heidi's getComponentCount does not agree with count returned from getComponents"));

	}
	
	
	((BattleMechPtr)mMech)->resetComponents(totalComponents,componentList);
	
	return moverHandle;
}

//---------------------------------------------------------------------------

void DEBUGWINS_removeGameObject (GameObjectPtr obj);

long Mission::removeMover (MoverPtr mover) {

	DEBUGWINS_removeGameObject((GameObjectPtr)mover);
	missionInterface->removeMover(mover);
	if (MPlayer) {
		MPlayer->removeFromLocalMovers(mover);
		MPlayer->removeFromMoverRoster(mover);
	}
	ObjectManager->freeMover(mover);
	return(0);
}

//---------------------------------------------------------------------------

void Mission::tradeMover (MoverPtr mover, long newTeamID, long newCommanderID, char* pilotFileName, char* brainFileName) {

	missionInterface->removeMover(mover);
	if (MPlayer) {
		MPlayer->removeFromLocalMovers(mover);
		MPlayer->removeFromPlayerMoverRoster(mover);
	}
	if (newCommanderID > -1) 
	{
		char realPilotName[256];
		strcpy(realPilotName,&(pilotFileName[MPlayer ? 4 : 3]));
		LogisticsPilot *lPilot = LogisticsData::instance->getPilot(realPilotName);
		mover->loadPilot(pilotFileName, brainFileName, lPilot);
	}
	ObjectManager->tradeMover(mover, newTeamID, newCommanderID);
	if (mover->sensorSystem)
		mover->sensorSystem->broken = false;
	if (MPlayer && (newCommanderID > -1)) {
		MPlayer->addToPlayerMoverRoster(newCommanderID, mover);
		if (newCommanderID == MPlayer->commanderID)
			MPlayer->addToLocalMovers(mover);
	}

	if (newCommanderID > -1) 
	{
		missionInterface->addMover(mover);
	}
}

//----------------------------------------------------------------------------

bool Mission::calcComplexDropZones (char* missionName, char dropZoneCID[MAX_MC_PLAYERS]) {

	for (long p = 0; p < MAX_MC_PLAYERS; p++)
		dropZoneCID[p] = -1;

	FullPathFileName missionFileName;
	missionFileName.init(missionPath,missionName,".fit");

	FitIniFile* missionFile = new FitIniFile;
	gosASSERT(missionFile != NULL);
	
	long result = missionFile->open(missionFileName);
	if (result != NO_ERR)
		STOP(("Unable to open Mission File %s",(char*)missionFileName));

	//------------------------------------------------------------
	// First, let's see how many teams and commanders there are...
	/*long maxTeamID = -1;
	long maxCommanderID = -1;
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		if (MPlayer->playerInfo[i].team > maxTeamID)
			maxTeamID = MPlayer->playerInfo[i].team;
		if (MPlayer->playerInfo[i].commanderID > maxCommanderID)
			maxCommanderID = MPlayer->playerInfo[i].commanderID;
	}
	*/
	long dropZoneSetup[MAX_MC_PLAYERS] = {-1, -1, -1, -1, -1 ,-1 ,-1 ,-1};
	result = missionFile->seekBlock("Parts");
	gosASSERT(result == NO_ERR);
	result = missionFile->readIdULong("NumParts",numParts);
	gosASSERT(result == NO_ERR);
	long i;
	if (numParts)
		for (i = 1; i < long(numParts + 1); i++) {
			char partName[12];
			sprintf(partName,"Part%d",i);
			
			//------------------------------------------------------------------
			// Find the object to load
			result = missionFile->seekBlock(partName);
			gosASSERT(result == NO_ERR);

			char teamID = -1;
			result = missionFile->readIdChar("TeamId", teamID);
			gosASSERT(result == NO_ERR);
			if ((teamID < 0) || (teamID >= MAX_TEAMS))
				STOP(("Mission.calcComplexDropZones: bad teamID"));

			char commanderID = -1;
			result = missionFile->readIdChar("CommanderId", commanderID);
			if (result != NO_ERR) {
				long cID;
				result = missionFile->readIdLong("CommanderId", cID);
				gosASSERT(result == NO_ERR);
				commanderID = (char)cID;
			}

			if ((commanderID < 0) || (commanderID >= MAX_MC_PLAYERS))
				STOP(("Mission.calcComplexDropZones: bad commanderID"));
			dropZoneSetup[commanderID] = teamID;
	}

	//------------------------------------------------------------------------------------
	// First, let's confirm that we have the correct player/team setup for this mission...
	long teamSize[MAX_TEAMS][2];
	for (i = 0; i < MAX_TEAMS; i++) {
		teamSize[i][0] = 0;
		teamSize[i][1] = 0;
	}
/*	for (i = 0; i < MAX_MC_PLAYERS; i++)
		if (dropZoneSetup[i] > -1)
			teamSize[dropZoneSetup[i]][0]++;
	for (i = 0; i < MAX_MC_PLAYERS; i++)
		if (MPlayer->playerInfo[i].commanderID > -1)
			teamSize[MPlayer->playerInfo[i].team][1]++;
	for (i = 0; i < MAX_MC_PLAYERS; i++)
		if (teamSize[i][0] != teamSize[i][1])
			return(false);
*/
	//---------------------------------------------------------
	// We know everything's good, so hand out the drop zones...
	for (i = 0; i < MAX_MC_PLAYERS; i++) {
		long index = RandomNumber(teamSize[MPlayer->playerInfo[i].team][0]--);
		for (long j = 0; j < MAX_MC_PLAYERS; j++)
			if (dropZoneSetup[j] == MPlayer->playerInfo[i].team) {
				if (index == 0) {
					dropZoneSetup[j] = -1;
					dropZoneCID[j] = i;
					break;
				}
				index--;
			}

	}

	missionFile->close();
	delete missionFile;
	missionFile = NULL;


	return(true);
}

//----------------------------------------------------------------------------

bool IsGateDisabled (long objectWID) {

	//--------------------------------------------------------------
	// Actually looks at the object and its parent, if it has one...
	if (objectWID < 1)
		return(false);
	GameObjectPtr obj = ObjectManager->getByWatchID(objectWID);
	if (obj && obj->isDisabled())
		return(true);
	if (obj->isBuilding()) {
		GameObjectPtr parent = ((BuildingPtr)obj)->getParent();
		if (parent)
			return(parent->isDisabled());
	}
	return(false);
}

//----------------------------------------------------------------------------

bool IsGateOpen (long objectWID) {

	//--------------------------------------------------------------
	// Actually looks at the object and its parent, if it has one...
	if (!objectWID)
		return(false);
	GameObjectPtr obj = ObjectManager->getByWatchID(objectWID);
	if (!obj)
		return(false);
	if (obj->getObjectClass() == GATE)
		return(((Gate*)obj)->opened);
	return(false);
}

//----------------------------------------------------------------------------
void Mission::init (char *missionName, long loadType, long dropZoneID, Stuff::Vector3D* dropZoneList, char commandersToLoad[8][3], long numMoversPerCommander)
{
	invulnerableON = false;

	terminationResult = mis_PLAYING; 

	// M8-B67: discard any objective bonus staged by a mission that was aborted or
	// lost. Only a win commits one, in LogisticsData::setMissionCompleted.
	if ( LogisticsData::instance )
		LogisticsData::instance->clearPendingBonusPurchaseFiles();

	//Start finding the Leaks
	//systemHeap->startHeapMallocLog();
	//systemHeap->dumpRecordLog();

	loadProgress = 0.0f;
	MC2_PumpLoadFrame();

	loadProgress = 1.0f;
	MC2_PumpLoadFrame();

	if ((loadType == MISSION_LOAD_SP_QUICKSTART) || (loadType == MISSION_LOAD_SP_LOGISTICS)) {
		char teamRelationsForSP[MAX_TEAMS][MAX_TEAMS] = {
			{0, 2, 1, 2, 2, 2, 2, 2},
			{2, 0, 2, 2, 2, 2, 2, 2},
			{1, 2, 0, 2, 2, 2, 2, 2},
			{2, 2, 2, 0, 2, 2, 2, 2},
			{2, 2, 2, 2, 0, 2, 2, 2},
			{2, 2, 2, 2, 2, 0, 2, 2},
			{2, 2, 2, 2, 2, 2, 0, 2},
			{2, 2, 2, 2, 2, 2, 2, 0}
		};
		long i;
		for (i = 0; i < MAX_TEAMS; i++)
			for (long j = 0; j < MAX_TEAMS; j++) {
				Team::relations[i][j] = teamRelationsForSP[i][j];
				TeamRelations[i][j] = teamRelationsForSP[i][j];
			}
		}
	else {
		char teamRelationsForMP[MAX_TEAMS][MAX_TEAMS] = {
			{0, 2, 2, 2, 2, 2, 2, 2},
			{2, 0, 2, 2, 2, 2, 2, 2},
			{2, 2, 0, 2, 2, 2, 2, 2},
			{2, 2, 2, 0, 2, 2, 2, 2},
			{2, 2, 2, 2, 0, 2, 2, 2},
			{2, 2, 2, 2, 2, 0, 2, 2},
			{2, 2, 2, 2, 2, 2, 0, 2},
			{2, 2, 2, 2, 2, 2, 2, 0}
		};
		long i;
		for (i = 0; i < MAX_TEAMS; i++)
			for (long j = 0; j < MAX_TEAMS; j++) {
				Team::relations[i][j] = teamRelationsForMP[i][j];
				TeamRelations[i][j] = teamRelationsForMP[i][j];
			}
	}
				
	//-------------------------------------------
	// Always reset turn at scenario start
	turn = 0;
	terminationCounterStarted = 0;

	#ifdef LAB_ONLY
	x=GetCycles();
	#endif

	//-----------------------
	// Init the ABL system...
	initABL();

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeABLLoad=x1-x;
#endif

	initBareMinimum();
	loadProgress = 4.0f;
	MC2_PumpLoadFrame();
	initTGLForMission();
	#if defined(__EMSCRIPTEN__)
		// PORT: Browser mission-load phase markers separate storage/terrain,
		// simulation objects, and interface startup in the bounded harness.
		std::printf("[mission-load] core classes ready\n");
	#endif
	
	//--------------------------------------------------------------
	// Start the Mission Heap
	missionHeap = new UserHeap;
	gosASSERT(missionHeap != NULL);
	
	missionHeap->init(missionHeapSize,"MISSION");
	
	//--------------------------
	// Load Game System stuff...
	FullPathFileName fullGameSystemName;
	fullGameSystemName.init(missionPath, "gamesys", ".fit");
	
	FitIniFile* gameSystemFile = new FitIniFile;
	gosASSERT(gameSystemFile != NULL);
		
	long result = gameSystemFile->open(fullGameSystemName);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->seekBlock("WeaponRanges");
	gosASSERT(result == NO_ERR);

	float span[2];
	result = gameSystemFile->readIdFloatArray("Short", span, 2);
	gosASSERT(result == NO_ERR);
	WeaponRanges[WEAPON_RANGE_SHORT][0] = span[0];
	WeaponRanges[WEAPON_RANGE_SHORT][1] = span[1];

	result = gameSystemFile->readIdFloatArray("Medium", span, 2);
	gosASSERT(result == NO_ERR);
	WeaponRanges[WEAPON_RANGE_MEDIUM][0] = span[0];
	WeaponRanges[WEAPON_RANGE_MEDIUM][1] = span[1];

	result = gameSystemFile->readIdFloatArray("Long", span, 2);
	gosASSERT(result == NO_ERR);
	WeaponRanges[WEAPON_RANGE_LONG][0] = span[0];
	WeaponRanges[WEAPON_RANGE_LONG][1] = span[1];

	result = gameSystemFile->readIdFloatArray("OptimalRangePoints", OptimalRangePoints, 5);
	gosASSERT(result == NO_ERR);

	long i;
	for (i = 0; i < 5; i++)
		for (long j = 0; j < 3; j++) {
			OptimalRangePointInRange[i][j] = false;
			if (OptimalRangePoints[i] > WeaponRanges[j][0])
				if (OptimalRangePoints[i] <= WeaponRanges[j][1])
					OptimalRangePointInRange[i][j] = true;
		}

	result = gameSystemFile->seekBlock("General");
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("MaxVisualRange",maxVisualRange);
	gosASSERT(result == NO_ERR);
	MaxVisualRadius = maxVisualRange * 1.4142;

	result = gameSystemFile->readIdFloat("FireVisualRange",fireVisualRange);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdULong("MaxTreeLOSBlock",MaxTreeLOSCellBlock);
	if (result != NO_ERR)
		MaxTreeLOSCellBlock = 5;

	result = gameSystemFile->readIdFloatArray("WeaponRange", WeaponRange, NUM_FIRERANGES);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("DefaultAttackRange", DefaultAttackRange);
	if (result != NO_ERR)
		DefaultAttackRange = 75.0;

	result = gameSystemFile->readIdFloat("BaseSensorRange",baseSensorRange);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdLongArray("VisualRangeTable",visualRangeTable,256);
	// PORT (M8-B45): IS THE VISUAL RANGE TABLE BEING READ AT ALL?
	//
	// Measured in the owner's own play session: the units shelling them sit at
	// 437-532 m, beyond their 400 m sensors, and GameObject::lineOfSight caps
	// seeing-by-eye at radius * 25 * worldUnitsPerMeter, which came out as a flat
	// 1,127 world units -- 225 m -- for every check in the run. A CONSTANT cutoff
	// means one table entry is being used, so print the table: a flat table, or
	// one that failed to load and left zeros or a default, is the bug; a table
	// that ramps with altitude clears it.
	if (MC2_SensorGateCensus)
	{
		long lo = visualRangeTable[0], hi = visualRangeTable[0], nz = 0;
		for (long q = 0; q < 256; ++q)
		{
			if (visualRangeTable[q] < lo) lo = visualRangeTable[q];
			if (visualRangeTable[q] > hi) hi = visualRangeTable[q];
			if (visualRangeTable[q]) ++nz;
		}
		printf("[visual-range] read result=%ld  min=%ld max=%ld nonzero=%ld of 256\n",
			(long)result, lo, hi, nz);
		printf("[visual-range]   samples [0]=%ld [1]=%ld [8]=%ld [32]=%ld [64]=%ld [128]=%ld [255]=%ld\n",
			visualRangeTable[0], visualRangeTable[1], visualRangeTable[8],
			visualRangeTable[32], visualRangeTable[64], visualRangeTable[128],
			visualRangeTable[255]);
		printf("[visual-range]   a value of V means eyes reach V * 25 metres\n");
		fflush(stdout);
	}
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("BaseHeadShotElevation",BaseHeadShotElevation);
	if (result != NO_ERR)
		BaseHeadShotElevation = 1.0f;

	long forestMoveCost;
	result = gameSystemFile->readIdLong("ForestMoveCost", forestMoveCost);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("MaxUnitExtractDistance",MaxExtractUnitDistance);
	if (result != NO_ERR)
		MaxExtractUnitDistance = 1280.0f;	//Ten Tiles away
		
	//----------------------------------------------------------------------
	// Now that we have some base values, load the master component table...
	if (!MasterComponent::masterList) {
		FullPathFileName compFileName;
		compFileName.init(objectPath,"compbas",".csv");
#ifdef _DEBUG
		long loadErr = 
#endif
			MasterComponent::loadMasterList(compFileName, 255, baseSensorRange);
		gosASSERT(loadErr == NO_ERR);
	}

	// PORT (M8-B45): the whole sensor table, once. A home Wolfhound reads a sensor
	// range of ONE metre from component 13 while other mechs read 400 from
	// component 14, and both are genuine COMPONENT_FORM_SENSOR entries. Range is
	// parsed as (csv multiplier * baseSensorRange), so both numbers are printed: a
	// table of sane values means the Wolfhound simply carries a weak sensor, and a
	// stray 1.0 among sane ones points at the parse instead.
	if (MC2_SensorGateCensus && MasterComponent::masterList)
	{
		printf("[comp-table] baseSensorRange=%.3f\n", baseSensorRange);
		// How far can a weapon actually reach? The owner is being shelled by units
		// that never come within 400 m of a sensor, so either something outranges
		// their sensors -- normal, and a design complaint -- or the shooter is close
		// and the census is missing it, which would be a defect. This number decides
		// which, and it is read from the same table the game fires with.
		{
			long maxW = -1, maxId = -1, over400 = 0;
			for (long c = 0; c < 255; c++)
			{
				const ComponentFormType f = MasterComponent::masterList[c].getForm();
				if ((f != COMPONENT_FORM_WEAPON) && (f != COMPONENT_FORM_WEAPON_ENERGY)
				    && (f != COMPONENT_FORM_WEAPON_BALLISTIC) && (f != COMPONENT_FORM_WEAPON_MISSILE))
					continue;
				const long r = MasterComponent::masterList[c].getWeaponRange();
				if (r > maxW) { maxW = r; maxId = c; }
				if (r > 400) ++over400;
			}
			printf("[comp-table] longest weapon range = %ld m (component %ld); %ld weapons reach past 400 m\n",
				maxW, maxId, over400);
		}
		for (long c = 0; c < 255; c++)
			if (MasterComponent::masterList[c].getForm() == COMPONENT_FORM_SENSOR)
				printf("[comp-table]   component %ld sensor range=%.2f m (csv multiplier %.4f)\n",
					c, MasterComponent::masterList[c].getSensorRange(),
					(baseSensorRange != 0.0f)
						? MasterComponent::masterList[c].getSensorRange() / baseSensorRange : -1.0f);
		fflush(stdout);
	}

	// PORT (M8-B33): HOW FAR A TERRAIN OBJECT STILL CASTS A SHADOW.
	//
	// Owner's request: stop spending render time on shadows out at the horizon,
	// and put the boundary at "a Raven's sensor range" -- the reach of the best
	// scout in the game. There is no cheaper shadow METHOD to fall back to: a
	// shadow costs a whole second TransformMultiShape of the dedicated shadow
	// shape every frame, so the only lever is cast or do not cast.
	//
	// The number is taken from the game's own data rather than typed in here:
	// the longest range of any SENSOR component in the master table, which is
	// the scout sensor, converted from metres to world units. Measured at the
	// horizon tilt on mc2_06, shadows are 48% of all terrain-object geometry and
	// 3.3 ms of an 11.5 ms frame.
	//
	// `ShadowDistance` in the loose system.cfg, under [CameraSettings], overrides
	// it; 0 there restores retail. MC2_SHADOW_DISTANCE overrides both, for sweeps.
	{
		float bestSensorMetres = 0.0f;
		if (MasterComponent::masterList)
		{
			for (long c = 0; c < 255; c++)
			{
				if (MasterComponent::masterList[c].getForm() != COMPONENT_FORM_SENSOR)
					continue;
				const float range = MasterComponent::masterList[c].getSensorRange();
				if (range > bestSensorMetres)
					bestSensorMetres = range;
			}
		}
		MC2_ShadowCutoff = bestSensorMetres * worldUnitsPerMeter;

		// system.cfg's value, read at startup in MechCmd2.cpp. Negative means the
		// file said nothing and the data default above stands. This used to read
		// gamesys.fit, which is packed inside an .fst -- the key was documented
		// as editable and was not.
		if (MC2_ShadowDistanceCfg >= 0.0f)
			MC2_ShadowCutoff = MC2_ShadowDistanceCfg;

		const char* envDistance = getenv("MC2_SHADOW_DISTANCE");
		if (envDistance && *envDistance)
			MC2_ShadowCutoff = (float)atof(envDistance);

		if (MC2_ShadowCutoff < 0.0f)
			MC2_ShadowCutoff = 0.0f;

		printf("[shadow-dist] terrain-object shadows cut off at %.1f world units"
		       " (best sensor %.1f m, %.4f world units/m)%s\n",
		       MC2_ShadowCutoff, bestSensorMetres, worldUnitsPerMeter,
		       MC2_ShadowCutoff > 0.0f ? "" : " -- DISABLED, everything casts");
		fflush(stdout);
	}

	result = gameSystemFile->readIdUChar("GodMode", godMode);
	if (result != NO_ERR)
		godMode = 0;

	unsigned char revealTacMap;
	result = gameSystemFile->readIdUChar("RevealTacMap", revealTacMap);
	if (result != NO_ERR)
		revealTacMap = 0;
		
	result = gameSystemFile->readIdUChar("FootPrints", footPrints);
	if (result != NO_ERR)
		footPrints = 1;

	result = gameSystemFile->readIdLong("BonusTonnageDivisor",tonnageDivisor);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLong("BonusPointsPerTon",resourcesPerTonDivided);
	gosASSERT(result == NO_ERR);
	
 	result = gameSystemFile->readIdFloat("CheatHitDamage",CheatHitDamage);
	if (result != NO_ERR)
		CheatHitDamage = 5.0f;
	
	//---------------------------------------
	// Read in difficulty here if it exits.
	InitDifficultySettings(gameSystemFile);

	result = Mover::loadGameSystem(gameSystemFile, maxVisualRange);
	gosASSERT(result == NO_ERR);

	//result = loadMultiplayerGameSystem(gameSystemFile);
	//gosASSERT(result == NO_ERR);

	result = BattleMech::loadGameSystem(gameSystemFile);
	gosASSERT(result == NO_ERR);

	//--------------------------------------------------------------------
	result = GroundVehicle::loadGameSystem(gameSystemFile);
	gosASSERT(result == NO_ERR);

#ifdef USE_ELEMENTALS
	result = loadElementalGameSystem(gameSystemFile);
	gosASSERT(result == NO_ERR);
#endif

	result = gameSystemFile->seekBlock("Mine");
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("BaseDamage", MineDamage);
	gosASSERT(result == NO_ERR);
		
	result = gameSystemFile->readIdFloat("SplashDamage", MineSplashDamage);
	gosASSERT(result == NO_ERR);
		
	result = gameSystemFile->readIdFloat("SplashRange", MineSplashRange);
	gosASSERT(result == NO_ERR);
		
	result = gameSystemFile->readIdLong("Explosion", MineExplosion);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdLong("MineLayThrottle", MineLayThrottle);
	if (result != NO_ERR)
		MineLayThrottle = 50;

	result = gameSystemFile->readIdLong("MineSweepThrottle", MineSweepThrottle);
	if (result != NO_ERR)
		MineSweepThrottle = 50;

	result = gameSystemFile->readIdFloat("MineWaitTime", MineWaitTime);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("StrikeWaitTime", StrikeWaitTime);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("StrikeTimeToImpact", StrikeTimeToImpact);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->seekBlock("Smoke");
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLong("MaxSmokeSpheres",totalSmokeSpheres);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdLong("TotalSmokeShapeSize",totalSmokeShapeSize);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->seekBlock("Fire");
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLong("MaxFiresBurning", maxFiresBurning);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdFloat("MaxFireBurnTime", maxFireBurnTime);
	gosASSERT(result == NO_ERR);

	memset(missionFileName,0,80);
	strncpy(missionFileName,missionName,79);

	FullPathFileName missionFileName;
	missionFileName.init(missionPath,missionName,".fit");

	duration = 60;
	
	missionFile = new FitIniFile;
	gosASSERT(missionFile != NULL);
	
	result = missionFile->open(missionFileName);
	if (result != NO_ERR)
		STOP(("Unable to open Mission File %s",(char*)missionFileName));

	if (!dropZoneList) {
		result = missionFile->seekBlock("Multiplayer");
		if (result == NO_ERR) {
			#if 0
			result = missionFile->readIdULong("TeamId", MultiPlayTeamId);
			gosASSERT(result == NO_ERR);
			result = missionFile->readIdULong("CommanderId", MultiPlayCommanderId);
			gosASSERT(result == NO_ERR);
			char sessionName[128];
			result = missionFile->readIdString("SessionName", sessionName, 127);
			gosASSERT(result == NO_ERR);
			char playerName[128];
			result = missionFile->readIdString("PlayerName", playerName, 127);
			gosASSERT(result == NO_ERR);
			bool isServer = false;
			result = missionFile->readIdBoolean("Server", isServer);
			gosASSERT(result == NO_ERR);
			unsigned long numPlayers;
			result = missionFile->readIdULong("NumPlayers", numPlayers);
			gosASSERT(result == NO_ERR);
			gosASSERT(MPlayer == NULL);
			MPlayer = new MultiPlayer;
			Assert(MPlayer != NULL, 0, " Unable to create MultiPlayer object ");
			MPlayer->setup();
			MPlayer->commanderID = MultiPlayCommanderId;
			//-------------------------------------------
			// If I'm the server, then create the game...
			if (isServer) {
				if (MPlayer->hostGame(sessionName, playerName, numPlayers)) {
					//---------------------------------------------
					//game hosted, so now wait for all check-ins...
					MPlayer->serverCID = MultiPlayCommanderId;//(MultiPlayCommanderId == ServerPlayerNum); //(gos_NetInformation(gos_AmITheServer) == 0);
				}
				}
			else {
				MPlayer->joinGame(NULL, sessionName, playerName);
				//MPlayer->numFitPlayers = numPlayers;
			}
			#endif
		}
	}

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMiscToTeamLoad=x-x1;
#endif

	//-----------------------------------
	// Find the SKY Number and save it.
	// If no number, i.e. an old mission file,
	// simply make it 1 until its written out
	// in the magical editor (tm)
	result = missionFile->seekBlock("TheSky");
	if (result != NO_ERR)
		theSkyNumber = DEFAULT_SKY;
	else
	{
		result = missionFile->readIdLong("SkyNumber",theSkyNumber);
		if (result != NO_ERR)
			theSkyNumber = DEFAULT_SKY;
			
		if ((theSkyNumber < 1) || (theSkyNumber > 21))
			theSkyNumber = DEFAULT_SKY;
	}
		
	//-----------------------------------------
	// Begin Setting up Teams and Commanders...
	loadProgress = 10.0f;
	MC2_PumpLoadFrame();

	result = missionFile->seekBlock("Teams");
	Assert(result == NO_ERR, result, " Could not find Teams Block ");
	for (i = 0; i < Team::numTeams; i++)
		if (Team::teams[i]) {
			delete Team::teams[i];
			Team::teams[i] = NULL;
		}
	Team::numTeams = 0;
	for (i = 0; i < Commander::numCommanders; i++)
		if (Commander::commanders[i]) {
			delete Commander::commanders[i];
			Commander::commanders[i] = NULL;
		}
	Commander::numCommanders = 0;

	//------------------------------------------------------------
	// First, let's see how many teams and commanders there are...
	long maxTeamID = -1;
	long maxCommanderID = -1;
	if (loadType == MISSION_LOAD_MP_LOGISTICS) {
		long i;
		for (i = 0; i < MAX_MC_PLAYERS; i++) {
			if (MPlayer->playerInfo[i].team > maxTeamID)
				maxTeamID = MPlayer->playerInfo[i].team;
			if (MPlayer->playerInfo[i].commanderID > maxCommanderID)
				maxCommanderID = MPlayer->playerInfo[i].commanderID;
		}
		}
	else {
		result = missionFile->seekBlock("Parts");
		gosASSERT(result == NO_ERR);
		result = missionFile->readIdULong("NumParts",numParts);
		gosASSERT(result == NO_ERR);
		if (numParts)
			for (i = 1; i < long(numParts + 1); i++) {
				char partName[12];
				sprintf(partName,"Part%d",i);
				
				//------------------------------------------------------------------
				// Find the object to load
				result = missionFile->seekBlock(partName);
				gosASSERT(result == NO_ERR);

				char teamID = -1;
				result = missionFile->readIdChar("TeamId", teamID);
				gosASSERT(result == NO_ERR);

				char commanderID = -1;
				result = missionFile->readIdChar("CommanderId", commanderID);
				if (result != NO_ERR) {
					long cID;
					result = missionFile->readIdLong("CommanderId", cID);
					gosASSERT(result == NO_ERR);
					commanderID = (char)cID;
				}

				if (MPlayer && dropZoneList) {
					//-------------------------------------------------------------
					// Since dropZoneList is not NULL, we know this was not started
					// from the command-line...
					long origCommanderID = commanderID;
					commanderID = commandersToLoad[origCommanderID][0];
					teamID = commandersToLoad[origCommanderID][1];
				}

				if (commanderID > maxCommanderID)
					maxCommanderID = commanderID;
				if (teamID > maxTeamID)
					maxTeamID = teamID;
			}
	}

	//----------------------------------------------
	// Now, init the teams and commanders we need...
	for (i = 0; i <= maxTeamID; i++) {
		Team::teams[i] = new Team;
		Team::teams[i]->init(i);
	}
	for (i = 0; i <= maxCommanderID; i++) {
		Commander::commanders[i] = new Commander;
		Commander::commanders[i]->setId(i);
	}
	
	if (MPlayer) {
		Team::home = Team::teams[MultiPlayTeamId];
		Commander::home = Commander::commanders[MultiPlayCommanderId];
		long i;
		for (i = 0; i <= maxCommanderID; i++)
			if (MPlayer->playerInfo[i].commanderID > -1)
				Commander::commanders[MPlayer->playerInfo[i].commanderID]->setTeam(Team::teams[MPlayer->playerInfo[i].team]);
		}
	else {
		Team::home = Team::teams[0];
		Commander::home = Commander::commanders[0];
		long i;
		for (i = 0; i <= maxCommanderID; i++) {
			if (commandersToLoad[i][0] > -1)
				Commander::commanders[commandersToLoad[i][0]]->setTeam(Team::teams[commandersToLoad[i][1]]);
		}
		Commander::commanders[0]->setTeam(Team::home);
	}

	//-----------------------------
	// Init Trigger Area Manager...
	if (Mover::triggerAreaMgr) {
		delete Mover::triggerAreaMgr;
		Mover::triggerAreaMgr = NULL;
	}
	Mover::triggerAreaMgr = new TriggerAreaManager;
	Assert(Mover::triggerAreaMgr != NULL, 0, " Mossion.init: unable to init triggerAreaMgr ");

	//-----------------------------------
	// Setup the Sensor System Manager...
	SensorManager = new SensorSystemManager;
	Assert(SensorManager != NULL, 0, " Unable to init sensor system manager ");
	if (MC2_SensorGateCensus)
	{
		printf("[sensor-mgr] new SensorSystemManager + init from %s\n", "MISSION START");
		fflush(stdout);
	}
	result = SensorManager->init(true);
	Assert(result == NO_ERR, result, " could not start Sensor System Manager ");

	result = missionFile->seekBlock( "DropZone0" );
	if ( result == NO_ERR ) // lets not enforce drop zones for now
	{
		missionFile->readIdFloat( "PositionX", dropZone.x );
		missionFile->readIdFloat( "PositionY", dropZone.y );
	}
	else
	{
		dropZone.x = -1.f;
		dropZone.y = -1.f;
	}

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeTeamLoad=x1-x;
#endif

	//-----------------------------------------------------------------
	// Load the names of the scenario tunes.
	//result = missionFile->seekBlock("Music");
	result = missionFile->seekBlock("MissionSettings");
	gosASSERT(result == NO_ERR);
		
	result = missionFile->readIdUChar("scenarioTuneNum",missionTuneNum);
	gosASSERT(result == NO_ERR);

	long numRPoints;
	result = missionFile->readIdLong("ResourcePoints",numRPoints);
	if (MaxResourcePoints > -1)
		numRPoints = MaxResourcePoints;
	if (MPlayer) {
		numRPoints = MPlayer->missionSettings.resourcePoints;
		long i;
		for (i = 0; i < MAX_MC_PLAYERS; i++) {
			MPlayer->playerInfo[i].resourcePoints = MPlayer->missionSettings.resourcePoints;
			MPlayer->playerInfo[i].resourcePointsGained = 0;
			MPlayer->playerInfo[i].resourcePointsAtStart = MPlayer->missionSettings.resourcePoints;
		}
	}
	LogisticsData::instance->setResourcePoints(numRPoints);

	if (MPlayer) {
		result = missionFile->readIdULong("NumRandomRPbuildings", MPlayer->numRandomResourceBuildings);
	}

	craterManager = (CraterManagerPtr)missionHeap->Malloc(sizeof(CraterManager));
	gosASSERT(craterManager != NULL);
		
	result = craterManager->init(1000,20479,"feet");
	gosASSERT(result == NO_ERR);
	
	//-----------------------------------------------------------------
	// Start the object system next.	
	ObjectManager = new GameObjectManager;
	if (!ObjectManager)
		Fatal(0, " Mission.init: unable to create ObjectManager ");
	ObjectManager->init("object2", 716799, 3072000);
	gosASSERT(result == NO_ERR);

	
	//-----------------------------------------------------------------
	// Start the collision detection system. -- Doesn't need objects?
	ObjectManager->initCollisionSystem(missionFile);

	//------------------------------------------------------------
	// Start the Terrain System

	FullPathFileName terrainFileName;
	terrainFileName.init( missionPath, missionName, ".pak" ); 

	PacketFile pakFile;
	result = pakFile.open( terrainFileName );
	gosASSERT( result == NO_ERR );

	land = new Terrain;

	land->getColorMapName(missionFile);

	gosASSERT(land != NULL);

	loadProgress = 15.0f;
	MC2_PumpLoadFrame();

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeObjectLoad=x-x1;
#endif

	long terrainInitResult = land->init(&pakFile, 0, GameVisibleVertices, loadProgress, 20.0 );

	if (terrainInitResult != NO_ERR)
	{
		STOP(("Could not load terrain.  Probably size was wrong!"));
	}

	loadProgress = 35.0f;
	MC2_PumpLoadFrame();

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeTerrainLoad=x1-x;
#endif

	land->load( missionFile );
	#if defined(__EMSCRIPTEN__)
		std::printf("[mission-load] terrain ready\n");
	#endif

	loadProgress = 36.0f;
	MC2_PumpLoadFrame();

//	land->recalcWater();		//Should have already been done in the editor

	//----------------------------------------------------
	// Start GameMap for Movement System
	Assert(SimpleMovePathRange > 20, SimpleMovePathRange, " Simple MovePath Range too small ");
	MOVE_init(SimpleMovePathRange);
	if (pakFile.seekPacket(4) == NO_ERR)
	{
		if (pakFile.getPacketSize() != 0)
		{
			MOVE_readData(&pakFile, 4);
			if (GlobalMoveMap[0]->badLoad)
				Fatal(0, " Mission.init: old version of move data (re-save map) ");
			GameMap->placeMoversCallback = PlaceMovers;
			GlobalMoveMap[0]->isGateDisabledCallback = IsGateDisabled;
			GlobalMoveMap[1]->isGateDisabledCallback = IsGateDisabled;
			GlobalMoveMap[2]->isGateDisabledCallback = IsGateDisabled;
			GlobalMoveMap[0]->isGateOpenCallback = IsGateOpen;
			GlobalMoveMap[1]->isGateOpenCallback = IsGateOpen;
			GlobalMoveMap[2]->isGateOpenCallback = IsGateOpen;
		}
		else
			STOP(("Mission has not movement Data.  QuickSaved Map?"));
	}

	PathFindMap[SECTOR_PATHMAP]->blockedDoorCallback = GetBlockedDoorCells;
	PathFindMap[SECTOR_PATHMAP]->placeStationaryMoversCallback = PlaceStationaryMovers;
	PathFindMap[SIMPLE_PATHMAP]->placeStationaryMoversCallback = PlaceStationaryMovers;
	PathFindMap[SECTOR_PATHMAP]->forestCost = forestMoveCost;
	PathFindMap[SIMPLE_PATHMAP]->forestCost = forestMoveCost;
	PathManager = new MovePathManager;


	loadProgress = 40.0f;
	MC2_PumpLoadFrame();

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMoveLoad=x-x1;
#endif

	//----------------------
	// Load ABL Libraries...
	long numErrors, numLinesProcessed;
	FullPathFileName libraryFileName;
	libraryFileName.init(missionPath, "orders", ".abx");
	ABLModulePtr library = ABLi_loadLibrary(libraryFileName, &numErrors, &numLinesProcessed);
	gosASSERT(library != NULL);

	FullPathFileName libraryFileName1;
	libraryFileName1.init(missionPath, "miscfunc", ".abx");
	library = ABLi_loadLibrary(libraryFileName1, &numErrors, &numLinesProcessed);
	gosASSERT(library != NULL);

	FullPathFileName libraryFileName2;
	libraryFileName2.init(missionPath, "corebrain", ".abx");
	library = ABLi_loadLibrary(libraryFileName2, &numErrors, &numLinesProcessed);
	gosASSERT(library != NULL);
	
	//---------------------------
	// Load the mission script...
	//-----------------------------------------------------------------
	// We now read in the mission Script File Name
	result = missionFile->seekBlock("Script");
	gosASSERT(result == NO_ERR);
	
	result = missionFile->readIdString("ScenarioScript",missionScriptName,79);
	gosASSERT(result == NO_ERR);

	FullPathFileName brainFileName;
	brainFileName.init(missionPath, missionScriptName, ".abl");
	
	missionScriptHandle = ABLi_preProcess(brainFileName, &numErrors, &numLinesProcessed);
	gosASSERT(missionScriptHandle >= 0);
	
	missionBrain = new ABLModule;
	gosASSERT(missionBrain != NULL);
		
#ifdef _DEBUG
	long brainErr = 
#endif
		missionBrain->init(missionScriptHandle);
	gosASSERT(brainErr == NO_ERR);
	
	missionBrain->setName("Mission");
	//MissionBrain->setStep(TRUE);

	missionBrainParams = new ABLParam;
	gosASSERT(missionBrainParams != NULL);

	missionBrainCallback = missionBrain->findFunction("handlemessage", TRUE);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeMissionABLLoad=x1-x;
#endif

	loadProgress = 41.0f;
	MC2_PumpLoadFrame();

	//-------------------------------------------
	// Load all MechWarriors for this mission...
	MechWarrior::setup();

	result = missionFile->seekBlock("Warriors");
	gosASSERT(result == NO_ERR);

	unsigned long numWarriors;
	result = missionFile->readIdULong("NumWarriors",numWarriors);
	gosASSERT(result == NO_ERR);

	bool loadBrainParameters = (result == NO_ERR);
	if (numWarriors) 
	{
		long i;
		for (i = 1; i <= numWarriors; i++) 
		{
			char warriorName[12];
			sprintf(warriorName,"Warrior%d",i);
			
			//-------------------------
			// Find the warrior to load
			result = missionFile->seekBlock(warriorName);
			Assert(result == NO_ERR, i, " Could not find Warrior Number Block ");

			char warriorFile[100];
			result = missionFile->readIdString("Profile", warriorFile, 99);
			Assert(result == NO_ERR, 0, " Could not find Warrior Profile in Warrior Number Block ");

			MechWarriorPtr pilot = MechWarrior::newWarrior();
			if (!pilot)
				STOP(("Too many pilots in this mission!"));
			
			//--------------------------------------
			// Load the mechwarrior into the mech...
			FullPathFileName pilotFullFileName;
			pilotFullFileName.init(warriorPath, warriorFile, ".fit");
			
			FitIniFile* pilotFile = new FitIniFile;
			gosASSERT(pilotFile != NULL);
		
			long result = pilotFile->open(pilotFullFileName);
			gosASSERT(result == NO_ERR);
			result = pilot->init(pilotFile);
			gosASSERT(result == NO_ERR);
			
			pilotFile->close();
			delete pilotFile;
			pilotFile = NULL;
			
			//----------------------------
			// Read in the Brain module...
			char moduleName[128];
			result = missionFile->readIdString("Brain", moduleName, 127);
			gosASSERT(result == NO_ERR);
			
			//------------------------------------------------------------
			// For now, all mplayer brains are pbrain. Need to change when
			// we allow ai brains in mplayer...
			long numErrors, numLinesProcessed;
			FullPathFileName brainFileName;
			if (MPlayer) {
				pilot->setBrainName("pbrain");
				brainFileName.init(warriorPath, "pbrain", ".abl");
				}
			else {
				pilot->setBrainName(moduleName);
				brainFileName.init(warriorPath, moduleName, ".abl");
			}
			
			long moduleHandle = ABLi_preProcess(brainFileName, &numErrors, &numLinesProcessed);
			gosASSERT(moduleHandle >= 0);
			
#ifdef _DEBUG
			long error = 
#endif
				pilot->setBrain(moduleHandle);
			gosASSERT(error == 0);
		}
	}

	if (loadBrainParameters) {
		//---------------------------------------------------------------
		// Load the brain parameter file and load 'em for each warrior...
		unsigned long i;
		for (i = 1; i <= numWarriors; i++) {
			result = MechWarrior::warriorList[i]->loadBrainParameters(missionFile, i);
			//Assert(result == NO_ERR, result, " Could not load Warrior Brain Parameters ");
		}
				
	}	
	
#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeWarriorLoad=x-x1;
#endif

	loadProgress = 43.0f;
	MC2_PumpLoadFrame();
	#if defined(__EMSCRIPTEN__)
		std::printf("[mission-load] warrior data ready\n");
	#endif

	//-----------------------------------------------------------------
  	// All systems are GO if we reach this point.  Now we need to
	// parse the scenario file for the Objects we need for this scenario
	// We then create each object and place it in the world at the 
	// position we read in with the frame we read in.
	result = missionFile->seekBlock("Parts");
	gosASSERT(result == NO_ERR);
		
	result = missionFile->readIdULong("NumParts",numParts);
	gosASSERT(result == NO_ERR);

	//--------------------------------------------------------------------------------
	// IMPORTANT NOTE: mission parts should always start with Part 1.
	// Part 0 is reserved as a "NULL" id for routines that reference the mission
	// parts. AI routines, Brain keywords, etc. use PART ID 0 as an "object not found"
	// error code. DO NOT USE PART 0!!!!!!! Start with Part 1...

#define MAX_SQUADS			256
#define	MAX_ALTERNATIVES	15
#define	USE_ALTERNATES

	loadProgress = 43.5f;
	MC2_PumpLoadFrame();

	long numMoversLoaded[MAX_MC_PLAYERS] = {0, 0, 0, 0, 0, 0, 0, 0};
	long numDropZonePositions = 0;
	if (numParts)
	{
		//-----------------------------------------------------
		// Since we leave part 0 unused, malloc numParts + 1...
		parts = (PartPtr)missionHeap->Malloc(sizeof(Part) * (numParts + 1));
		gosASSERT(parts != NULL);
		
		memset(parts,0,sizeof(Part) * (numParts + 1));

#ifdef USE_ALTERNATES
		//------------------------------------------------------------
		// Before we actually read in the parts, do some prep work for
		// determining squad alternatives...
		long numSquads = 0;
		long squadMap[MAX_SQUADS];
		long i;
		for (i = 0; i < MAX_SQUADS; i++)
			squadMap[i] = -1;
		long maxAlternatives[MAX_SQUADS];
		long randomAlternative[MAX_SQUADS];
		long s;
		for (s = 0; s < MAX_SQUADS; s++) {
			maxAlternatives[s] = 0;
			randomAlternative[s] = -1;
		}

		long bigAlternatives = 0;

		for (i = 1; i < long(numParts + 1); i++) {
			char partName[12];
			sprintf(partName,"Part%d",i);

			result = missionFile->seekBlock(partName);
			gosASSERT(result == NO_ERR);
			unsigned long squadNum;
			result = missionFile->readIdULong("squadNum", squadNum);
			long squadIndex = 0;
			for (squadIndex = 0; squadIndex < numSquads; squadIndex++)
				if (squadMap[squadIndex] == squadNum)
					break;
			if (squadIndex == numSquads)
				squadMap[numSquads++] = squadNum;

			long alternatives[MAX_ALTERNATIVES];
			result = missionFile->readIdLongArray("IndicesOfAlternatives", alternatives, MAX_ALTERNATIVES);
			long numAlternatives = 0;
			for (numAlternatives = 0; numAlternatives < MAX_ALTERNATIVES; numAlternatives++)
				if (alternatives[numAlternatives] == -1)
					break;
			if (maxAlternatives[squadIndex] < numAlternatives)
				maxAlternatives[squadIndex] = numAlternatives;

			if (numAlternatives > bigAlternatives)
				bigAlternatives = numAlternatives;
		}

		if (MPlayer && (bigAlternatives > 0)) {
			PAUSE(("Mission.init: multiplayer map has random squads"));
			bigAlternatives = 0;
		}

		long alternateChoice = RandomNumber(bigAlternatives + 1);
		for (s = 0; s < numSquads; s++)
		{
			randomAlternative[s] = alternateChoice;
			if (GameDifficulty >= 2)
			{
				randomAlternative[s]--;
				if (randomAlternative[s] < 1)
					randomAlternative[s] = 1;
			}
		}

		//--------------------------------------------------------
		// This block is optional, and is used for testing only...
		result = missionFile->seekBlock("Squads");
		if (result == NO_ERR)
			for (i = 0; i < numSquads; i++) {
				char s[128];
				sprintf(s, "Squad%d", i);
				unsigned long alternate = -1;
				result = missionFile->readIdULong(s, alternate);
				if (result == NO_ERR)
					randomAlternative[i] = alternate;
			}
#else
		long i;
#endif

		for (i = 1; i < long(numParts + 1); i++)
		{
			char partName[12];
			sprintf(partName,"Part%d",i);
			
			//------------------------------------------------------------------
			// Find the object to load
			result = missionFile->seekBlock(partName);
			gosASSERT(result == NO_ERR);

#ifdef USE_ALTERNATES
			//----------------------------------------------------------------------
			// If we have alternatives, choose which one we're taking before we read
			// anything else in...
			bool usingAlternate = false;
			unsigned long realPilot = 0;
			unsigned long squadNum;
			result = missionFile->readIdULong("squadNum", squadNum);
			parts[i].squadId = squadNum;
			long squadIndex = 0;
			for (squadIndex = 0; squadIndex < numSquads; squadIndex++)
				if (squadMap[squadIndex] == squadNum)
					break;

			long alternatives[MAX_ALTERNATIVES];
			result = missionFile->readIdLongArray("IndicesOfAlternatives", alternatives, MAX_ALTERNATIVES);
			gosASSERT(result == NO_ERR);
			if (maxAlternatives[squadIndex]) 
			{
				long partId = i;
				if (randomAlternative[squadIndex] > 0)
					partId = alternatives[randomAlternative[squadIndex] - 1];
				if (partId == -1)
					continue;
				Assert(partId > 0, partId, " Mission.init: Bad Alternate ");

				//MUST save off ORIGINAL Pilot.  WE don't load the alternate pilots!!!!!
				usingAlternate = true;
				result = missionFile->readIdULong("Pilot", realPilot);
				gosASSERT(result == NO_ERR);

				sprintf(partName, "Part%d", partId);
				result = missionFile->seekBlock(partName);
				gosASSERT(result == NO_ERR);
			}
#endif

			//------------------------------------------------------------------
			// Find out what kind of object this is.
			result = missionFile->readIdULong("ObjectNumber",parts[i].objNumber);
			gosASSERT(result == NO_ERR);

			//-------------------------------------------------
			// Read in the data needed to control the object...
			result = missionFile->readIdULong("ControlType", parts[i].controlType);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdULong("ControlDataType", parts[i].controlDataType);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdString("ObjectProfile", parts[i].profileName, 9);
			gosASSERT(result == NO_ERR);
				
			result = missionFile->readIdULong("VariantNumber", parts[i].variantNum);
			if (result != NO_ERR)
				parts[i].variantNum = 0;		//FOR NOW!!!!!!!!!!!!!!!!
												//MAKE a REAL error when Heidi fixes editor.
												//-fs 12/7/99

			if (usingAlternate)
			{
				parts[i].pilot = realPilot;
			}
			else
			{
				result = missionFile->readIdULong("Pilot", parts[i].pilot);
				gosASSERT(result == NO_ERR);
			}
			
			//------------------------------------------------------------------
			// Read the object's position, initial velocity and rotation.
			result = missionFile->readIdFloat("PositionX",parts[i].position.x);
			gosASSERT(result == NO_ERR);
				
			result = missionFile->readIdFloat("PositionY",parts[i].position.y);
			gosASSERT(result == NO_ERR);
				
			parts[i].position.z = -1.0;

			result = missionFile->readIdFloat("Rotation",parts[i].rotation);
			gosASSERT(result == NO_ERR);
				
			result = missionFile->readIdChar("TeamId",parts[i].teamId);
			gosASSERT(result == NO_ERR);
			//--------------------------------------------------------------------------
			// Hack for singleplayer, until editor spits this out properly for allies...
			if (!MPlayer && (parts[i].teamId == 2))
				parts[i].teamId = 0;
			
			if (dropZoneList && (dropZoneID == parts[i].teamId))
				dropZoneList[numDropZonePositions++] = parts[i].position;

			char cmdId = 0;
			result = missionFile->readIdChar("CommanderId", cmdId);
			if (result != NO_ERR)
			{
				result = missionFile->readIdLong("CommanderId", parts[i].commanderID);
				gosASSERT(result == NO_ERR);
			}
			else
			{
				parts[i].commanderID = cmdId;
			}

			if (loadType == MISSION_LOAD_MP_QUICKSTART) {
				long origCommanderID = parts[i].commanderID;
				parts[i].commanderID = commandersToLoad[origCommanderID][0];
				parts[i].teamId = commandersToLoad[origCommanderID][1];
				if (commandersToLoad[origCommanderID][0] > -1) {
					if (numMoversLoaded[commandersToLoad[origCommanderID][0]] == numMoversPerCommander) {
						parts[i].commanderID = -1;
						parts[i].teamId = -1;
						}
					else
						numMoversLoaded[commandersToLoad[origCommanderID][0]]++;
				}
			}

			parts[i].gestureId = 2; // this has never changed
	
			result = missionFile->readIdULong("BaseColor",parts[i].baseColor);
			if (result != NO_ERR || MPlayer )
				parts[i].baseColor = prefs.baseColor;
				
			result = missionFile->readIdULong("HighlightColor1",parts[i].highlightColor1);
			if (result != NO_ERR || MPlayer)
				parts[i].highlightColor1 = prefs.highlightColor;
				
			result = missionFile->readIdULong("HighlightColor2",parts[i].highlightColor2);
			if (result != NO_ERR || MPlayer )
				parts[i].highlightColor2 = prefs.highlightColor;
				
  			parts[i].velocity = 0;
			
			result = missionFile->readIdLong("Active",parts[i].active);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdLong("Exists",parts[i].exists);
			gosASSERT(result == NO_ERR);

			float fDamage = 0.0f;
			result = missionFile->readIdFloat("Damage",fDamage);
			if (result == NO_ERR) {
				if (fDamage >= 1.0) {
					parts[i].destroyed = true;
				};
			}

			result = missionFile->readIdChar("MyIcon", parts[i].myIcon);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdChar("MyIcon", parts[i].myIcon);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdBoolean("Captureable", parts[i].captureable);
			if (result != NO_ERR)
				parts[i].captureable = FALSE;

			float increment = 5.0f/(numParts + 1);
			loadProgress += increment;
			MC2_PumpLoadFrame();

		}
	}

	loadProgress = 48.5f;
	MC2_PumpLoadFrame();
	//--------------------------------------------------------------------------
	// Now that the parts data has been loaded, let's prep the ObjectManager for
	// the real things. First, count the number of objects we need...
	long numMechs = 0;
	long numVehicles = 0;
	for (i = 1; i < (numParts + 1); i++) 
	{
		ObjectTypePtr objType = ObjectManager->loadObjectType(parts[i].objNumber);
		if (!objType)
			objType = ObjectManager->getObjectType(parts[i].objNumber);
		if (objType)
			switch (objType->getObjectTypeClass()) 
			{
				case BATTLEMECH_TYPE:
					numMechs++;
					break;
				case VEHICLE_TYPE:
					numVehicles++;
					break;
			}

		float increment = 6.5f/(numParts + 1);
		loadProgress += increment;
		MC2_PumpLoadFrame();
	}

	switch (loadType) 
	{
		case MISSION_LOAD_SP_QUICKSTART:
		case MISSION_LOAD_SP_LOGISTICS:
			break;
		case MISSION_LOAD_MP_QUICKSTART:
		case MISSION_LOAD_MP_LOGISTICS:
			numMechs = 64;
			numVehicles = 64;
			break;
	}

	loadProgress = 55.0f;
	MC2_PumpLoadFrame();

	pakFile.seekPacket( 1 );
	ObjectManager->countTerrainObjects(&pakFile, (numMechs + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM) + (numVehicles + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM)/* + ObjectManager->maxElementals*/ + 1);
	loadProgress = 58.0f;
	MC2_PumpLoadFrame();
	ObjectManager->setNumObjects(numMechs, numVehicles, 0, -1, -1, -1, 100, 50, 0, 130, -1);

	//-------------------------
	// Load the mech objects...
	long curMech = 0;
	long curVehicle = 0;
	for (long t = 0; t < 8; t++)
		for (i = 1; i < (numParts + 1); i++) 
		{
			bool loadEm = true;
			if (loadType == MISSION_LOAD_MP_LOGISTICS)
				loadEm = false;
			if (loadType == MISSION_LOAD_SP_LOGISTICS)
				if (parts[i].commanderID == 0 && !parts[i].destroyed)
					loadEm = false;
			if (loadType == MISSION_LOAD_MP_QUICKSTART)
				if (parts[i].commanderID == -1)
					loadEm = false;
			if (loadEm) {
				ObjectTypePtr objType = ObjectManager->getObjectType(parts[i].objNumber);
				if (objType)
					switch (objType->getObjectTypeClass()) 
					{
						case BATTLEMECH_TYPE:
							if (parts[i].teamId == t) 
							{
								BattleMechPtr mech = ObjectManager->getMech(curMech++);
								createPartObject(i, mech);
								float increment = 10.0f/(numParts + 1);
								loadProgress += increment;
								MC2_PumpLoadFrame();
							}
							break;
						case VEHICLE_TYPE:
							if (parts[i].teamId == t) 
							{
								GroundVehiclePtr vehicle = ObjectManager->getVehicle(curVehicle++);
								createPartObject(i, vehicle);
								float increment = 10.0f/(numParts + 1);
								loadProgress += increment;
								MC2_PumpLoadFrame();
							}
							break;
					}
				}
			else 
			{
				MechWarrior::freeWarrior(MechWarrior::warriorList[parts[i].pilot]);
			}
		}

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeMoverPartsLoad=x1-x;
#endif

	loadProgress = 68.0f;
	MC2_PumpLoadFrame();

	ObjectManager->loadTerrainObjects(&pakFile, loadProgress, 30);

	loadProgress = 98.0f;
	MC2_PumpLoadFrame();
	#if defined(__EMSCRIPTEN__)
		std::printf("[mission-load] objects and objectives ready\n");
	#endif

	ObjectManager->buildMoverLists();

	if (MPlayer)
		MPlayer->initSpecialBuildings(commandersToLoad);

	//----------------------------------------------
	// Read in the Mission Time Limit.
	{
		m_timeLimit = -1.0f	/*seconds  IF -1.0 mission never times out!!! -fs*/;
		long result = 0;
		result = missionFile->seekBlock("MissionSettings");
		if (NO_ERR == result)
		{
			float tmpFloat = 0.0;
			result = missionFile->readIdFloat("TimeLimit", tmpFloat);
			if (NO_ERR == result)
			{
				m_timeLimit = tmpFloat;
			}
		}

		if ( MPlayer )
			m_timeLimit = MPlayer->missionSettings.timeLimit;
	}

	if (NumDisableAtStart) {
		long i;
		for (i = 0; i < NumDisableAtStart; i++)
			ObjectManager->getByWatchID(parts[DisableAtStart[i]].objectWID)->setDebugFlag(OBJECT_DFLAG_DISABLE, true);
	}

	//----------------------------------------------
	// Read in the Objectives.  Safe to have none.
	for (i = 0; i <= maxTeamID; i++)
		Team::teams[i]->loadObjectives(missionFile);

/*	numObjectives = 0; // this refers to the number of *old* objectives
	objectives = 0;

	warning1 = FALSE;
	warning2 = FALSE;

	Team::home->objectives.Clear();
	Team::home->objectives.Read(missionFile);

	Team::home->numPrimaryObjectives = 0;
	CObjectives::EIterator it = Team::home->objectives.Begin();
	while (!it.IsDone())
	{
		if (1 == (*it)->Priority())
		{
			Team::home->numPrimaryObjectives += 1;
		}
		it++;
	}
	ReadNavMarkers(missionFile, Team::home->objectives);
*/
#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeObjectiveLoad=x-x1;
#endif

	//----------------------------
	// Read in Commander Groups...
	for (long curCommanderId = 0; curCommanderId < MAX_MC_PLAYERS; curCommanderId++) {
		long numGroups = 0;
		char headingStr[128];
		
		if (commandersToLoad[curCommanderId][0] == -1)
			continue;

		sprintf(headingStr, "Commander%dGroup:%d", curCommanderId, numGroups);
		result = missionFile->seekBlock(headingStr);
		while (result == NO_ERR) {
			//---------------------------
			// Read in the Group Mates...
			bool pointSet = FALSE;
			long groupMates[MAX_MOVERGROUP_COUNT_START];
			result = missionFile->readIdLongArray("Mates", groupMates, MAX_MOVERGROUP_COUNT_START);
			Assert(result == NO_ERR, result, " could not find Mates in Group in Scenario File ");
			for (long curMate = 0; curMate < MAX_MOVERGROUP_COUNT_START; curMate++) {
				if ((groupMates[curMate] > 0) && parts[groupMates[curMate]].objectWID) {
					ObjectManager->setPartId(ObjectManager->getByWatchID(parts[groupMates[curMate]].objectWID), commandersToLoad[curCommanderId][0], numGroups, curMate);
					Commander::commanders[commandersToLoad[curCommanderId][0]]->getGroup(numGroups)->add((MoverPtr)ObjectManager->getByWatchID(parts[groupMates[curMate]].objectWID));
					if (!pointSet) {
						Commander::commanders[commandersToLoad[curCommanderId][0]]->getGroup(numGroups)->selectPoint(true);
						pointSet = true;
					}
				}
			}
			
			numGroups++;
			sprintf(headingStr, "Commander%dGroup:%d", curCommanderId, numGroups);
			result = missionFile->seekBlock(headingStr);
		}
	}

	//-----------------------------------------------------------------------
	// Now that the parts are loaded, let's build the roster for each team...
	for (i = 0; i < Team::numTeams; i++)
		Team::teams[i]->buildRoster();

	//---------------------------------------------------------------------------------
	// If we're not playing multiplayer, make sure all home commander movers have their
	// localMoverId set to 0, so the iface can at least check if a mover is player
	// controlled...
	if (!MPlayer)
		Commander::home->setLocalMoverId(0);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeCommanderLoad=x1-x;
#endif

	//-----------------------------------------------------
	// This tracks time since scenario started in seconds.
	LastTimeGetTime = 0xffffffff;
	scenarioTime = 0.0;
	MissionStartTime = 0;
	runningTime = 0.0;
	actualTime = 0.0;

	gameSystemFile->close();
	delete gameSystemFile;
	gameSystemFile = NULL;

	//----------------------------
	// Create and load the Weather
	weather = new Weather;
	weather->init(missionFile);
	#if defined(__EMSCRIPTEN__)
		// PORT: Bound browser mission-load stalls to the camera/GUI phases.
		std::printf("[mission-load] world ready\n");
	#endif
	
 	//---------------------------------------------------------------
	// Start the Camera and Lists
	eye = new GameCamera;
	eye->init();
	gosASSERT(eye != NULL);

	result = eye->init(missionFile);
	gosASSERT(result == NO_ERR);
	#if defined(__EMSCRIPTEN__)
		std::printf("[mission-load] camera ready\n");
	#endif

	eye->inMovieMode = false;

	loadProgress = 99.0;
	MC2_PumpLoadFrame();

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMiscLoad=x-x1;
#endif

	//----------------------------------------------------------------------------
	// Start the Mission GUI
	missionInterface = new MissionInterfaceManager;
	gosASSERT(missionInterface != NULL);
	
	missionInterface->initTacMap( &pakFile, 2 );
	#if defined(__EMSCRIPTEN__)
		std::printf("[mission-load] tactical map ready\n");
	#endif
	
	FullPathFileName missionScreenName;
	missionScreenName.init(artPath,"missionScrn",".fit");
	
	FitIniFile missionLoader;
	result = missionLoader.open(missionScreenName);
	gosASSERT(result == NO_ERR);
	
	missionInterface->init(&missionLoader);
	#if defined(__EMSCRIPTEN__)
		std::printf("[mission-load] interface layout ready\n");
	#endif
	missionInterface->initMechs();
	#if defined(__EMSCRIPTEN__)
		std::printf("[mission-load] interface mechs ready\n");
	#endif
	missionLoader.close();
	
	//----------------------------------------------------------------------------
	userInput->setMouseCursor(mState_NORMAL);

	loadProgress = 100.0;
	MC2_PumpLoadFrame();

	
	//MechWarrior::initGoalManager(200);

	if (tempSpecialAreaFootPrints) {
		systemHeap->Free(tempSpecialAreaFootPrints);
		tempSpecialAreaFootPrints = NULL;
		tempNumSpecialAreas = 0;
	}

	Mover::initOptimalCells(32);

	//--------------------------------------------------
	// Close all walls and open gates and landbridges...
//	GameObjectPtr wallObjects[MAX_WALL_OBJECTS];
//	long numWalls = ObjectManager->getSpecificObjects(BUILDING, BUILDING_SUBTYPE_WALL, wallObjects, MAX_WALL_OBJECTS);
//	for (i = 0; i < numWalls; i++)
//		((BuildingPtr)wallObjects[i])->closeSubAreas();
//	long numLandBridges = ObjectManager->getSpecificObjects(BUILDING, BUILDING_SUBTYPE_LANDBRIDGE, wallObjects, MAX_WALL_OBJECTS);
//	for (i = 0; i < numLandBridges; i++)
//		((BuildingPtr)wallObjects[i])->openSubAreas();
//	for (i = 0; i < ObjectManager->getNumGates(); i++) {
//		GatePtr gate = ObjectManager->getGate(i);
//		gate->openSubAreas();
//	}

	if (CombatLog)
		MechWarrior::logPilots(CombatLog);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeGUILoad=x1-x;
#endif

#ifdef LAB_ONLY	
	//Add Mission Load statistics to GameOS Debugger screen!
	MCTimeABLLoad       *= OneOverProcessorSpeed;
	MCTimeMiscToTeamLoad*= OneOverProcessorSpeed;
	MCTimeTeamLoad      *= OneOverProcessorSpeed;
	MCTimeObjectLoad    *= OneOverProcessorSpeed;
	MCTimeTerrainLoad   *= OneOverProcessorSpeed;
	MCTimeMoveLoad      *= OneOverProcessorSpeed;
	MCTimeMissionABLLoad*= OneOverProcessorSpeed;
	MCTimeWarriorLoad   *= OneOverProcessorSpeed;
	MCTimeMoverPartsLoad*= OneOverProcessorSpeed;
	MCTimeObjectiveLoad *= OneOverProcessorSpeed;
	MCTimeCommanderLoad *= OneOverProcessorSpeed;
	MCTimeMiscLoad      *= OneOverProcessorSpeed;
	MCTimeGUILoad       *= OneOverProcessorSpeed;
#endif

	missionFile->close();
	delete missionFile;
	missionFile = NULL;
}

//----------------------------------------------------------------------------------
// Sets mission active.  Assumes that whatever we needed to do to the mission data
// set for logistis has been done and player is ready to play!
void DEBUGWINS_setGameObject (long debugObj, GameObjectPtr obj);
extern long GameObjectWindowList[3];
extern long NumGameObjectsToDisplay;

void Mission::start (void)
{
	active = true;

	// PORT INSTRUMENT (M8-B65): the census used to hang off the -mission command
	// line only, so a normal campaign launch -- the only way the owner plays --
	// printed nothing at all. First playtest wasted for that reason. Every path
	// into a mission comes through here.
	{
		extern void MC2_ObjectiveCensus (const char* when);
		MC2_ObjectiveCensus("mission start");
	}
	{
		extern void MC2_MissionProbe (void);
		MC2_MissionProbe();
	}
	long i;
	for (i = 0; i < NumGameObjectsToDisplay; i++)
		DEBUGWINS_setGameObject(-1, ObjectManager->getByWatchID(parts[GameObjectWindowList[i]].objectWID));
}

//----------------------------------------------------------------------------------
void Mission::initTGLForMission()
{
	//---------------------------------------------------------
	// End the Tiny Geometry Layer Heap for Logistics
	if (TG_Shape::tglHeap)
	{
		//Shut down the TGL RAM pools.
		if (colorPool)
		{
			colorPool->destroy();
			delete colorPool;
			colorPool = NULL;
		}
		
		if (vertexPool)
		{
			vertexPool->destroy();
			delete vertexPool;
			vertexPool = NULL;
		}

		if (facePool)
		{
			facePool->destroy();
			delete facePool;
			facePool = NULL;
		}

		if (shadowPool)
		{
			shadowPool->destroy();
			delete shadowPool;
			shadowPool = NULL;
		}

		if (trianglePool)
		{
			trianglePool->destroy();
			delete trianglePool;
			trianglePool = NULL;
		}
		
		TG_Shape::tglHeap->destroy();

		delete TG_Shape::tglHeap;
		TG_Shape::tglHeap = NULL;
	}

	loadProgress += 1.0f;
	MC2_PumpLoadFrame();

	//------------------------------------------------------
	// Start the Tiny Geometry Layer Heap for the Mission.
	if ( !TG_Shape::tglHeap )
	{
		//---------------------------------------------------------
		// PORT (M8-B15): +16 MB to cover the resized pools below, which grow from
		// about 2.2 MB to about 7.2 MB. Retail sized this heap for retail pools.
		// PORT (M8-B32): 56 -> 64 MB. The parallel object workers went from four
		// 1.0 MB slices to one shared 9.4 MB pool, and the pools are cut from
		// this heap. Malloc failure here is a gosASSERT, which is compiled out
		// of RelWithDebInfo, so an overrun would surface as a null pool and a
		// crash rather than a message. The 8 MB covers the 5.4 MB growth twice.
		// PORT (M8-B45): 64 -> 128 MB. The main pools go 7.2 -> 28.7 MB on the
		// owner's "remove the cap" ruling, so pools alone are now 28.7 + 9.4 =
		// 38.1 MB of this heap and the rest of TGL still has to fit beside them.
		// Doubling keeps the same kind of margin the 64 MB step bought, and the
		// failure mode if it is ever too small is the bad one named above: a
		// null pool, because the assert is compiled out of RelWithDebInfo.
		unsigned long tglHeapSize = 128 * 1024 * 1024;

		TG_Shape::tglHeap = new UserHeap;
		TG_Shape::tglHeap->init(tglHeapSize,"TinyGeom");

		// PORT (M8-B9): a new heap means every shape pointer cached against
		// the previous one is dead. See AppearanceTypeList::getAppearance.
		TG_Shape::tglGeneration++;
		

		//Start up the TGL RAM pools.
		// PORT (M8-B15): THE PER-FRAME TGL POOLS, RESIZED FOR THIS PORT'S VIEW
		// DISTANCE. These five pools are bump allocators reset every frame, and
		// TG_Shape::MultiTransformShape returns EARLY AND SILENTLY when any of
		// them is empty -- the shape transforms nothing, draws zero faces, and
		// every visibility check upstream still reports it visible. That silence
		// is M8-B15: whole outposts, units and enemies vanishing depending on
		// where the camera looked.
		//
		// The sizes below WERE retail's, and so were the pools -- but retail fed
		// them far less geometry. Measured against pristine-microsoft:
		//     retail  MaxClipDistance max 5,000   GameVisibleVertices max 60
		//     port    MaxClipDistance up to 18,000  GameVisibleVertices up to 160
		// so the port can put several times as much geometry in frame as the
		// pools were ever sized for. Nothing in the port's code was wrong in
		// isolation; the view distance was extended and these were not.
		//
		// MEASURED PEAK DEMAND in one frame (mc2_06, the owner's save, camera at
		// the top-left outpost, MC2_POOL_CENSUS=1):
		//     facing 0 deg    vertices 23,429   triangles 23,403   -> 266 shapes lost
		//     facing 180 deg  vertices 33,469   triangles 26,691   -> 57,946 shapes lost
		// The triangle pool overflowed even at the rotation that LOOKED correct.
		//
		// Sized ~3.2x retail, which clears the measured peak with headroom for
		// denser scenes. Cost is small and exact: TG_Vertex 4, gos_VERTEX 32,
		// TG_Triangle 24, shadow 16, DWORD 4 bytes -- 2.2 MB before, 7.2 MB after,
		// which is why tglHeapSize grows with them.
		//
		// facePool is drawn from TWICE per shape (visible faces AND visible
		// shadows), so it must stay at least 2x the triangle pool.
		//
		// MC2_POOL_CENSUS=1 reports any remaining starvation and the peak demand.
		// It should read zero; if it ever does not, these numbers are too small
		// again and the symptom is objects silently disappearing.
		// PORT (M8-B45): 4x AGAIN, ON THE OWNER'S RULING OF 2026-09-08 --
		// "missions have pre-set unit spawns and we don't need a cap anymore
		// with modern hardware."
		//
		// WHY IT HAD TO MOVE, measured in their own invisible-tank session:
		//     [pool] main pool: high water verts= 46595 tris= 63211
		//            of capacity verts= 96000 tris= 64000 faces=160000
		// TRIANGLES AT 98.8% -- 789 from the cap -- while vertices sat at 48.5%.
		// The sizes above were already 3.2x retail and still nearly out, and the
		// failure mode is the one described above: a shape that cannot get pool
		// space transforms nothing and draws zero faces while every visibility
		// check still calls it visible. Silently invisible units.
		//
		// THE POOLS WERE ALSO MIS-PROPORTIONED. Triangles bound at 98.8% while
		// vertices had half their space free, so the vertex pools were holding
		// headroom the triangle pool needed.
		//
		// facePool is still drawn from TWICE per shape (visible faces AND
		// visible shadows), so it stays well above 2x the triangle pool.
		//
		// Cost, exact, from the per-entry sizes listed above: 96000*(4+32+16) +
		// 64000*24 + 160000*4 = 7.2 MB becomes 384000*52 + 256000*24 +
		// 640000*4 = 28.7 MB. tglHeapSize grows to cover it.
		//
		// NOT A LICENCE TO STOP MEASURING: MC2_POOL_CENSUS still reports the
		// high water, and if it ever approaches these numbers the symptom is
		// again units silently disappearing.
		colorPool 		= new TG_VertexPool;
		colorPool->init(384000);

		vertexPool 		= new TG_GOSVertexPool;
		vertexPool->init(384000);

		facePool 		= new TG_DWORDPool;
		facePool->init(640000);

		shadowPool 		= new TG_ShadowPool;
		shadowPool->init(384000);

		trianglePool 	= new TG_TrianglePool;
		trianglePool->init(256000);

		// PORT (M8-B32): the census used to PRINT these three numbers from a
		// fixed string, so every run reported them even when the pool that
		// actually ran dry was a parallel worker's. Register them instead.
		MC2_PoolNoteCapacity(0, 384000, 256000, 640000);
	}

	loadProgress += 4.0f;
	MC2_PumpLoadFrame();

	// PORT (M8-3): the persisted graphics option owns the existing global. This
	// keeps every retail fog producer in agreement and applies without restart.
	useFog = prefs.useFog;
	useShadows = prefs.useShadows;
}


//----------------------------------------------------------------------------------
void Mission::destroy (bool initLogistics)
{
	// PORT: teardown census (M7 A1b). Every MC2_TeardownCheckpoint call in this
	// function is port instrumentation, not original Microsoft code. Each one
	// names the phase that has just FINISHED, then validates the GameOS block
	// guards and every host heap.
	//
	// The corruption here is reported by __fastfail(FAST_FAIL_HEAP_CORRUPTION),
	// which bypasses the exception filter -- no stack, no crash report, and no
	// trailing output. So the trail has to be flushed as it is laid, and the
	// answer is read as: last phase printed = survived, next phase = suspect.
	//
	// Silent and effectively free unless MC2_TEARDOWN_CENSUS is set.
	MC2_TeardownCheckpoint("entry");	// PORT: census

	//---------------------------------------------------------------
	// Shutdown the Mission Interface
	if (missionInterface)
	{
		missionInterface->destroy();
		
		delete missionInterface;
		missionInterface = NULL;
	}
	
	MC2_TeardownCheckpoint("missionInterface");	// PORT: census

	delete missionBrain;
	missionBrain = NULL;

	delete missionBrainParams;
	missionBrainParams = NULL;

	//Team::home->objectives.Clear();

	//---------------------------------------------------------------
	// End the Camera and Lists
	MC2_TeardownCheckpoint("missionBrain");	// PORT: census

	if (eye)
	{
		eye->destroy();

		delete eye;
		eye = NULL;
	}

	MC2_TeardownCheckpoint("camera");	// PORT: census

	if (PathManager) {
		PathManager->destroy();
		delete PathManager;
		PathManager = NULL;
	}
	MC2_TeardownCheckpoint("PathManager");	// PORT: census

	MOVE_cleanup();

	MC2_TeardownCheckpoint("MOVE_cleanup");	// PORT: census

	MechWarrior::shutdown();
	
	MC2_TeardownCheckpoint("MechWarrior::shutdown");	// PORT: census

	delete weather;
	weather = NULL;

	//------------------------------------------------------------
	// End the Terrain System
	MC2_TeardownCheckpoint("weather");	// PORT: census

	if (land)
	{
		land->destroy();

		delete land;
		land = NULL;
	}

	//--------------------------------------------------------------
	// Shut down the object system
	// CANNOT just delete heaps do to our ASSHOLE MUNGA/ADEPT
	// CODE which cannot be shutdown and restarted more then once.
	//
	// I wish I could just do demos and call that game experience!!!!
	//
	MC2_TeardownCheckpoint("land (terrain)");	// PORT: census

	if (ObjectManager)
	{
		ObjectManager->destroy();
		delete ObjectManager;
		ObjectManager = NULL;
	}
	
	MC2_TeardownCheckpoint("ObjectManager");	// PORT: census

	if (Mover::sortList)
	{
		MC2_TeardownExpectLive("Mover::sortList", Mover::sortList);	// PORT: census
		delete Mover::sortList;
		Mover::sortList = NULL;
	}

	if (SensorSystem::sortList)
	{
		MC2_TeardownExpectLive("SensorSystem::sortList", SensorSystem::sortList);	// PORT: census
		delete SensorSystem::sortList;
		SensorSystem::sortList = NULL;
	}

	MC2_TeardownCheckpoint("Mover/Sensor sortLists");	// PORT: census

	if (GVAppearanceType::SensorTriangleShape)
	{
		MC2_TeardownExpectLive("GVAppearanceType::SensorTriangleShape", GVAppearanceType::SensorTriangleShape);	// PORT: census
		delete GVAppearanceType::SensorTriangleShape;
		GVAppearanceType::SensorTriangleShape = NULL;
	}
	
	if (GVAppearanceType::SensorCircleShape)
	{
		MC2_TeardownExpectLive("GVAppearanceType::SensorCircleShape", GVAppearanceType::SensorCircleShape);	// PORT: census
		delete GVAppearanceType::SensorCircleShape;
		GVAppearanceType::SensorCircleShape = NULL;
	}

	if (Mech3DAppearanceType::SensorSquareShape)
	{
		MC2_TeardownExpectLive("Mech3DAppearanceType::SensorSquareShape", Mech3DAppearanceType::SensorSquareShape);	// PORT: census
		delete Mech3DAppearanceType::SensorSquareShape;
		Mech3DAppearanceType::SensorSquareShape = NULL;
	}
	
	MC2_TeardownCheckpoint("GV+Mech sensor shapes");	// PORT: census

	if (MasterComponent::masterList) 
	{
		MasterComponent::freeMasterList();
	}

	//-----------------------------------
	// Get rid of the Effects manager
	MC2_TeardownCheckpoint("MasterComponent::freeMasterList");	// PORT: census

	if (weaponEffects)
		weaponEffects->destroy();

	delete weaponEffects;
	weaponEffects = NULL;

	MC2_TeardownCheckpoint("weaponEffects");	// PORT: census

	if ( craterManager )
	{
		craterManager->destroy();
		missionHeap->Free( craterManager );
		craterManager = NULL;
	}

	//-----------------------------------
	// Sensors now stored in the missionHeap
	// Which goes away several lines below this
	//if (SensorManager)
	//{
	//	SensorManager->destroy();
	//	delete SensorManager;
	//	SensorManager = NULL;
	//}

	MC2_TeardownCheckpoint("craterManager");	// PORT: census

	long numt = Team::numTeams;
	long i;
	for (i = 0; i < numt; i++)
		if (Team::teams[i]) {
			delete Team::teams[i];
			Team::teams[i] = NULL;
		}
	Team::numTeams = 0;

	MC2_TeardownCheckpoint("Team deletes");	// PORT: census

	long numC = Commander::numCommanders;
	for (i = 0; i < numC; i++)
		if (Commander::commanders[i]) {
			delete Commander::commanders[i];
			Commander::commanders[i] = NULL;
		}
	Commander::numCommanders = 0;

	MC2_TeardownCheckpoint("Commander deletes");	// PORT: census

	if (Mover::triggerAreaMgr) {
		delete Mover::triggerAreaMgr;
		Mover::triggerAreaMgr = NULL;
	}

	if (MoverGroup::goalMap)
	{
		systemHeap->Free(MoverGroup::goalMap);
		MoverGroup::goalMap = NULL;
	}

	MC2_TeardownCheckpoint("Mover::triggerAreaMgr");	// PORT: census

	if (openList)
	{
		delete openList;
		openList = NULL;
	}

	if (Team::sortList)
	{
		delete Team::sortList;
		Team::sortList = NULL;
	}

	MC2_TeardownCheckpoint("goalMap");	// PORT: census

	closeABL();

	//------------------------------------------------------------
	// End the Mission Heap
	MC2_TeardownCheckpoint("closeABL");	// PORT: census

	if (missionHeap)
	{
		missionHeap->destroy();
		
		delete missionHeap;
		missionHeap = NULL;
	}
	
	MC2_TeardownCheckpoint("missionHeap");	// PORT: census

	if ( appearanceTypeList )
	{
		appearanceTypeList->destroy();
		delete appearanceTypeList;
		appearanceTypeList = NULL;
	}
	
	MC2_TeardownCheckpoint("appearanceTypeList");	// PORT: census

	if (initLogistics)
		initTGLForLogistics();
	else
	{
		//---------------------------------------------------------
		// End the Tiny Geometry Layer Heap for Logistics
		if (TG_Shape::tglHeap)
		{
			//Shut down the TGL RAM pools.
			if (colorPool)
			{
				colorPool->destroy();
				delete colorPool;
				colorPool = NULL;
			}

			if (vertexPool)
			{
				vertexPool->destroy();
				delete vertexPool;
				vertexPool = NULL;
			}

			if (facePool)
			{
				facePool->destroy();
				delete facePool;
				facePool = NULL;
			}

			if (shadowPool)
			{
				shadowPool->destroy();
				delete shadowPool;
				shadowPool = NULL;
			}

			if (trianglePool)
			{
				trianglePool->destroy();
				delete trianglePool;
				trianglePool = NULL;
			}

			TG_Shape::tglHeap->destroy();

			delete TG_Shape::tglHeap;
			TG_Shape::tglHeap = NULL;
		}
	}
	
//	userInput->mouseOff();

	MC2_TeardownCheckpoint("TGL pools");	// PORT: census

	active = FALSE;
	scenarioResult = mis_PLAYING;		//We've completed the mission.  Set it back to playing!
	GeneralAlarm = false;

	EllipseElement::removeTextureHandle();

	//The flush below will remove this one.
	Mover::holdFireIconHandle = 0;

	MC2_TeardownCheckpoint("EllipseElement + icon handles");	// PORT: census

	mcTextureManager->flush();
	mcTextureManager->freeVertices();

	MC2_TeardownCheckpoint("mcTextureManager flush");	// PORT: census

	soundSystem->purgeSoundSystem();

	missionFileName[0] = 0;
	
	MC2_TeardownCheckpoint("soundSystem purge");	// PORT: census

	if (CObjective::s_markerFont)
	{
		delete CObjective::s_markerFont;
		CObjective::s_markerFont = NULL;
	}

	//Heading back to logistics now.  Change screen back to 800x600
//	if (renderer == 3)
//		gos_SetScreenMode(800,600,16,0,0,0,true,false,0,false,0,renderer);
//	else
//		gos_SetScreenMode(800,600,16,0,0,0,0,false,0,false,0,renderer);

	//Start finding the Leaks
	//systemHeap->dumpRecordLog();
	MC2_TeardownCheckpoint("CObjective::s_markerFont");	// PORT: census

}

//----------------------------------------------------------------------------------

void Mission::handleMultiplayMessage (long code, long param1) {

	if (missionBrainCallback) {
		CurMultiplayCode = code;
		CurMultiplayParam = param1;
		missionBrain->execute(NULL, missionBrainCallback);
		CurMultiplayCode = 0;
		CurMultiplayParam = 0;
	}
}

//----------------------------------------------------------------------------------

void Mission::startObjectiveTimers (void) {

	long i;
	for (i=0;i<(long)numObjectives;i++)
	{
		if (objectives[i].timeLeft > 0.0)
		{
			setObjectiveTimer(i,objectives[i].timeLeft);
		}
	}
}

//----------------------------------------------------------------------------------
long Mission::setObjectiveTimer (long objectiveNum, float timeLeft)
{
	gosASSERT((objectiveNum >= 0) && objectiveNum < (long)numObjectives);
		
	long timerNumber = OBJECTIVE_1_TIMER + objectiveNum;
	
	//------------
	// Add Timer.
	TimerPtr timer = timerManager->getTimer(timerNumber);
	gosASSERT(timer != NULL);
	timer->setTimer(timeLeft);

	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
float Mission::checkObjectiveTimer (long objectiveNum)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);

	long timerNumber = OBJECTIVE_1_TIMER + objectiveNum;
	unsigned long timeLeft = 0;

	TimerPtr timer = timerManager->getTimer(timerNumber);
	gosASSERT(timer != NULL);

	timeLeft = timer->getCurrentTime();
	
	return(timeLeft);
}	

//----------------------------------------------------------------------------------
long Mission::setObjectiveStatus (long objectiveNum, ObjectiveStatus status)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
	
	if (objectives)
		objectives[objectiveNum].status = status;
	
	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
bool Mission::checkObjectiveSuccess (void)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	bool result = false;
	if (Team::home->objectives.Count())
	{
		for ( CObjectives::EIterator iter = Team::home->objectives.Begin(); !iter.IsDone(); iter++, count++ )
		{
			//Must iterate through ALL objectives and check.
			result |= (*iter)->StatusChangedSuccess();
		}
	}
	
	return result;
}

//----------------------------------------------------------------------------------
bool Mission::checkObjectiveFailed (void)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	bool result = false;
	if (Team::home->objectives.Count())
	{
		for ( CObjectives::EIterator iter = Team::home->objectives.Begin(); !iter.IsDone(); iter++, count++ )
		{
			//Must iterate through ALL objectives and check.
			result |= (*iter)->StatusChangedFailed();
		}
	}
	
	return result;
}

//----------------------------------------------------------------------------------
ObjectiveStatus Mission::checkObjectiveStatus (long objectiveNum)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	if (Team::home->objectives.Count())
	{
		for ( CObjectives::EIterator iter = Team::home->objectives.Begin(); !iter.IsDone(); iter++, count++ )
		{
			if (count == objectiveNum)
			{
				return (*iter)->Status(Team::home->objectives);
			}
		}
	}
	
	return OS_UNDETERMINED;
}	

//----------------------------------------------------------------------------------
long Mission::setObjectiveType (long objectiveNum, ObjectiveType type)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);

	if (objectives)
		objectives[objectiveNum].type = type;
	
	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
ObjectiveType Mission::checkObjectiveType (long objectiveNum)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
		
	ObjectiveType result = PrimaryGoal;
	
	if (objectives)
		result = objectives[objectiveNum].type;
	
	return(result);
}	

//----------------------------------------------------------------------------------
void Mission::setObjectivePos (long objectiveNum, float realX, float realY, float realZ)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
		
	if (objectives)
	{
		objectives[objectiveNum].position.x = realX;
		objectives[objectiveNum].position.y = realY;
		objectives[objectiveNum].position.z = realZ;
	}
}


//---------------------------------------------------------------------------


