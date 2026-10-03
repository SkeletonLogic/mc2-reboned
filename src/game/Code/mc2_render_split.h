//===========================================================================//
// PORT (M8-B18): THE RENDER SPLIT.
//
// The frame split in gos_platform.cpp put UpdateRenderers at 2.42 ms of an
// 8.29 ms frame -- the largest single item outside the terrain-object phase,
// and the only one that had never been examined. This says WHICH part of it is
// the cost, so the next step is chosen on a measurement rather than a guess.
//
// SILENT unless MC2_RENDER_SPLIT is non-zero. It ACCUMULATES ONLY and never
// prints per frame, and that distinction is not cosmetic: MC2_LOGIC_PHASE_MS at
// a low threshold cost 12 ms a frame in per-turn PRINTING while its clock reads
// were free, which made the game look like it ran at 48.9 fps against a true
// 120.8. A probe that prints inside the loop measures its own printf.
//
// TOP-LEVEL phases are the direct children of UpdateRenderers and sum to it.
// NESTED phases live inside MC2_RP_MISSION and are listed for detail; they are
// deliberately excluded from the accounted total so nothing is counted twice.
//===========================================================================//
#ifndef MC2_RENDER_SPLIT_H
#define MC2_RENDER_SPLIT_H

#include <chrono>

enum
{
	// top level, children of UpdateRenderers
	MC2_RP_STATE = 0,      // the gos_SetRenderState / viewport prologue
	MC2_RP_MISSION,        // mission->render()
	MC2_RP_LOGISTICS,      // logistics->render() (menus only)
	MC2_RP_USERINPUT,      // userInput->render(), the cursor
	MC2_RP_DEBUGWINS,      // DEBUGWINS_render()
	MC2_RP_TOPLEVEL_COUNT,

	// nested inside MC2_RP_MISSION
	MC2_RP_EYE = MC2_RP_TOPLEVEL_COUNT,   // eye->render(), the world
	MC2_RP_DEVTOOLS,                      // MC2_DevToolsRender()
	MC2_RP_INTERFACE,                     // missionInterface->render(), the HUD

	// nested inside MC2_RP_EYE (GameCamera::render). Everything before
	// renderLists only BUILDS triangle lists; renderLists is what actually
	// sends them to the GPU.
	MC2_RP_SKY,
	MC2_RP_LAND,
	MC2_RP_CRATERS,
	MC2_RP_OBJECTS,
	MC2_RP_WATER,
	MC2_RP_SHADOWS,
	MC2_RP_COMPASS,
	MC2_RP_RENDERLISTS,
	MC2_RP_COUNT
};

extern double MC2_RenderPhaseTotal[MC2_RP_COUNT];
extern long   MC2_RenderPhaseFrames;

bool MC2_RenderSplitArmed (void);
void MC2_RenderPhaseAdd (long id, double ms);
void MC2_RenderPhaseFrame (void);

// Wraps one phase. When the probe is off this expands to the bare call and no
// clock is read, exactly like MC2_PHASE in mission.cpp.
#define MC2_RENDER_PHASE(id, ...)                                              \
	do {                                                                       \
		if (MC2_RenderSplitArmed())                                            \
		{                                                                      \
			const std::chrono::steady_clock::time_point mc2RpT0 =              \
				std::chrono::steady_clock::now();                              \
			__VA_ARGS__;                                                       \
			MC2_RenderPhaseAdd((id), std::chrono::duration<double, std::milli>( \
				std::chrono::steady_clock::now() - mc2RpT0).count());          \
		}                                                                      \
		else { __VA_ARGS__; }                                                  \
	} while (0)

#endif
