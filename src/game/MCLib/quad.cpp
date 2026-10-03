//---------------------------------------------------------------------------
//
// Quad.cpp -- File contains class code for the Terrain Quads
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef QUAD_H
#include "quad.h"
#endif

#ifndef VERTEX_H
#include "Vertex.h" // PORT: match case-sensitive host path.
#endif

#ifndef TERRAIN_H
#include "Terrain.h" // PORT: match case-sensitive host path.
#endif

#ifndef CAMERA_H
#include "Camera.h" // PORT: match case-sensitive host path.
#endif

#ifndef DBASEGUI_H
#include "dbasegui.h"
#endif

#ifndef CELINE_H
#include "celine.h"
#endif

#ifndef MOVE_H
#include "Move.h" // PORT: match case-sensitive host path.
#endif

#include <stdio.h>  // PORT (M8-B11 reopened): terrain-light output tally.
#include <stdlib.h>

// PORT (M8-B11 REOPENED 2026-08-19). The owner replayed the mission and the
// lights are still absent, which contradicts the earlier conclusion that
// nothing was wrong. That conclusion verified the PLUMBING -- lights created,
// classified, and a loop that consumes them -- and never measured the OUTPUT.
// "The call runs and returns success" is not evidence that anything reached the
// screen.
//
// This counts what actually lands on a terrain vertex: how many times a light
// was tested against a vertex, how many of those produced a falloff at all, the
// largest falloff seen, and the breakdown by lightType. If hits are zero while
// tests are large, the pools are computed and thrown away. If hits are zero
// because tests are zero, this code is not running at all in the configuration
// being measured -- which is a different fault and must not be confused with
// the first.
static long  mc2TerrainLightTests = 0;
static long  mc2TerrainLightHits = 0;
static float mc2TerrainLightMaxFalloff = 0.0f;
static long  mc2TerrainLightByType[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static void MC2_TerrainLightTally (TG_LightPtr light, bool lit, float falloff)
{
	static const bool wantCensus = (getenv("MC2_LIGHT_CENSUS") != NULL);
	if (!wantCensus)
		return;

	++mc2TerrainLightTests;
	if (lit && light)
	{
		++mc2TerrainLightHits;
		const long t = (long)light->lightType;
		mc2TerrainLightByType[(t >= 0 && t < 8) ? t : 7]++;
		if (falloff > mc2TerrainLightMaxFalloff)
			mc2TerrainLightMaxFalloff = falloff;
	}

	if ((mc2TerrainLightTests % 400000) == 0)
	{
		fprintf(stderr,
			"[light-census] terrain vertex lighting: tests=%ld hits=%ld maxFalloff=%.4f "
			"hitsByType 3=%ld 4=%ld 5=%ld\n",
			mc2TerrainLightTests, mc2TerrainLightHits, mc2TerrainLightMaxFalloff,
			mc2TerrainLightByType[3], mc2TerrainLightByType[4],
			mc2TerrainLightByType[5]);
		fflush(stderr);
	}
}

#define SELECTION_COLOR 0xffff7fff
#define HIGHLIGHT_COLOR	0xff00ff00

//---------------------------------------------------------------------------
float TerrainQuad::rainLightLevel = 1.0f;
DWORD TerrainQuad::lighteningLevel = 0;
DWORD TerrainQuad::mineTextureHandle = 0xffffffff;
DWORD TerrainQuad::blownTextureHandle = 0xffffffff;

extern bool drawTerrainGrid;
extern bool drawLOSGrid;
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// Class TerrainQuad
long TerrainQuad::init (VertexPtr v0, VertexPtr v1, VertexPtr v2, VertexPtr v3)
{
	vertices[0] = v0;
	vertices[1] = v1;
	vertices[2] = v2;
	vertices[3] = v3;

	return(NO_ERR);
}

float twoFiveFive = 255.0;
#define HUD_DEPTH		0.0001f			//HUD Objects draw over everything else.
extern float cosineEyeHalfFOV; 
extern DWORD BaseVertexColor;
extern bool useShadows;
extern bool useFog;
extern long sprayFrame;
bool useWaterInterestTexture = true;
bool useOverlayTexture = true;
long numTerrainFaces = 0;
extern float MaxMinUV;

extern float leastZ;
extern float leastW;
extern float mostZ; 
extern float mostW;
extern float leastWY; 
extern float mostWY;

//---------------------------------------------------------------------------
// PORT: B4 water census (2026-08-14). The water textures are known to LOAD --
// measured, 725 loads all open=0, mc2_01.water.tga among them -- so the defect
// is downstream. This answers, in order: does setupTextures reach the branch
// that assigns the water handles at all; are the handles it gets valid; does
// drawWater run; and does it pass its own handle check to submit geometry.
//
// EVERY COUNTER CAN COME BACK ZERO, WHICH IS THE POINT. branch=0 means the
// water is never set up and nothing downstream matters. valid=0 with branch>0
// moves the fault into terrainTextures2. draw=0 with valid>0 means the quads
// are never asked to draw. textured>0 with the owner still seeing untextured
// water puts the fault past this file entirely, in the renderer.
//
// Armed with MC2_WATER_CENSUS=1; silent otherwise, so the strict bar is
// unaffected. One summary line at exit.
struct MC2WaterCensus
{
	unsigned long setupCalls;
	unsigned long branch;          // reached the handle assignment
	unsigned long handleValid;
	unsigned long handleInvalid;
	unsigned long drawCalls;
	unsigned long drawTextured;    // passed waterHandle != 0xffffffff
	unsigned long drawSkipped;
	DWORD firstWater;
	DWORD firstDetail;
	bool  sampled;
	bool  registered;
};
static MC2WaterCensus mc2WaterCensus = { 0,0,0,0,0,0,0,0xffffffff,0xffffffff,false,false };

static bool MC2_WaterCensusOn (void)
{
	static const char* env = getenv("MC2_WATER_CENSUS");
	static const bool on = env && *env && strcmp(env, "0") != 0;
	return on;
}

static void MC2_WaterCensusDump (void)
{
	fprintf(stderr,
		"[water] setup=%lu branch=%lu valid=%lu invalid=%lu "
		"draw=%lu textured=%lu skipped=%lu firstWater=0x%08lx firstDetail=0x%08lx "
		"terrainTextures2=%d interest=%d\n",
		mc2WaterCensus.setupCalls, mc2WaterCensus.branch,
		mc2WaterCensus.handleValid, mc2WaterCensus.handleInvalid,
		mc2WaterCensus.drawCalls, mc2WaterCensus.drawTextured,
		mc2WaterCensus.drawSkipped,
		(unsigned long)mc2WaterCensus.firstWater,
		(unsigned long)mc2WaterCensus.firstDetail,
		Terrain::terrainTextures2 ? 1 : 0,
		useWaterInterestTexture ? 1 : 0);
	fflush(stderr);
}

static void MC2_WaterCensusArm (void)
{
	if (!mc2WaterCensus.registered)
	{
		mc2WaterCensus.registered = true;
		atexit(MC2_WaterCensusDump);
	}
}

// M8-B12, 2026-08-20. Owner screenshot: roads render as a FLAT UNTEXTURED BAND
// while the terrain around them carries detail. Before any fix, establish which
// of three things is true, because they need different work:
//
//   (a) the map says there is no overlay on those tiles at all;
//   (b) the map says there is one and this file never resolves a handle;
//   (c) a handle resolves and the draw happens, so the fault is downstream in
//       the renderer or in the overlay texture itself.
//
// READ BEFORE SUSPECTING THE PORT. Measured 2026-08-20 by diffing this file
// against pristine-microsoft: the overlay logic is IDENTICAL to retail apart
// from ONE added early-out, the out-of-map placeholder-vertex guard from the
// M7-B14 render-window work, which zeroes every handle including overlayHandle.
// That guard is the only port-side way overlays can be lost here, so it is
// counted separately and first.
//
// Also measured by reading: overlayHandle is assigned a REAL value at exactly
// two sites in this file, both inside the isAlpha "cement in the old universe"
// branch. Every other assignment is 0xffffffff. If roads depend on the overlay
// path at all, that is already a finding; if they are drawn by terrainTextures2
// instead, this census will show overlays at zero for a reason that is not a
// defect. DO NOT ASSUME WHICH -- that is what the counters are for.
//
// Armed with MC2_ROAD_CENSUS=1; silent otherwise, so the strict bar is
// unaffected. One summary line at exit.
struct MC2RoadCensus
{
	unsigned long setupCalls;
	unsigned long outOfMapSuppressed;   // the port-added guard fired
	unsigned long suppressedByBadCount[5];  // how many of the 4 were invalid
	unsigned long overlayQuadByFlags[4];    // [isCement*2 + isAlpha]
	unsigned long discardBySite[16];        // road thrown away, by source line
	unsigned long discardSiteLine[16];
	unsigned long discardSites;
	unsigned long culledAllQuads;   // control: same sites, every quad
	unsigned long mapHasOverlay;        // textureData >> 16 != 0xffff
	unsigned long handleResolved;       // overlayHandle got a real value
	unsigned long overlayDrawn;         // reached addVertices for the overlay
	DWORD         firstOverlayWord;     // first non-empty high word seen
	bool          registered;
};
static MC2RoadCensus mc2RoadCensus = { 0,0,{0,0,0,0,0},{0,0,0,0},
									{0},{0},0,0,0,0,0,0xffffffff,false };

// M8-B12, 2026-08-21. Aggregate counters cannot distinguish neighbouring
// road quads. MC2_ROAD_TRACE=<n> follows the terrain quad containing the n-th
// road cell (the same numbering used by MC2_CAMERA_GOTO_ROAD) frame by frame.
// A frame boundary report is emitted even when the target quad is absent, so a
// silent/broken probe cannot be mistaken for a negative result.
struct MC2RoadTrace
{
	long wanted;
	long cellRow;
	long cellCol;
	long tileRow;
	long tileCol;
	unsigned long frame;
	bool initialized;
	bool enabled;
	bool resolved;
	bool registered;
	bool frameStarted;
	bool setupSeen;
	bool drawCalled;
	DWORD uvMode;
	long clipped1;
	long clipped2;
	bool isCement;
	bool isAlpha;
	DWORD textureIndex;
	DWORD transitionType;
	DWORD transitionOverlay;
	bool transitionFound;
	DWORD terrainHandle;
	DWORD overlayHandle;
	float screenX;
	float screenY;
	unsigned long overlayAddVertices;
};

static MC2RoadTrace mc2RoadTrace = {
	0,-1,-1,-1,-1,0,false,false,false,false,false,false,false,
	0,0,0,false,false,0xffffffff,0,0,false,
	0xffffffff,0xffffffff,-1.0f,-1.0f,0
};

static void MC2_RoadTraceEmit (void)
{
	if (!mc2RoadTrace.enabled || !mc2RoadTrace.resolved || !mc2RoadTrace.frameStarted)
		return;
	fprintf(stderr,
		"[road-trace] frame=%lu road=%ld cell=%ld,%ld tile=%ld,%ld "
		"setup=%d uvMode=%ld clipped1=%ld clipped2=%ld clippedAny=%d "
		"isCement=%d isAlpha=%d textureIndex=%lu transition=%08lu.%08lu%s "
		"terrainHandle=0x%08lx overlayHandle=0x%08lx "
		"screen=%.1f,%.1f draw=%d overlayAddVertices=%lu\n",
		mc2RoadTrace.frame, mc2RoadTrace.wanted,
		mc2RoadTrace.cellRow, mc2RoadTrace.cellCol,
		mc2RoadTrace.tileRow, mc2RoadTrace.tileCol,
		mc2RoadTrace.setupSeen ? 1 : 0,
		(long)mc2RoadTrace.uvMode,
		mc2RoadTrace.clipped1, mc2RoadTrace.clipped2,
		(mc2RoadTrace.clipped1 || mc2RoadTrace.clipped2) ? 1 : 0,
		mc2RoadTrace.isCement ? 1 : 0, mc2RoadTrace.isAlpha ? 1 : 0,
		(unsigned long)mc2RoadTrace.textureIndex,
		(unsigned long)mc2RoadTrace.transitionType,
		(unsigned long)mc2RoadTrace.transitionOverlay,
		mc2RoadTrace.transitionFound ? "" : "(not-found)",
		(unsigned long)mc2RoadTrace.terrainHandle,
		(unsigned long)mc2RoadTrace.overlayHandle,
		mc2RoadTrace.screenX, mc2RoadTrace.screenY,
		mc2RoadTrace.drawCalled ? 1 : 0, mc2RoadTrace.overlayAddVertices);
	fflush(stderr);
}

static void MC2_RoadTraceAtExit (void)
{
	MC2_RoadTraceEmit();
}

static bool MC2_RoadTraceOn (void)
{
	if (!mc2RoadTrace.initialized)
	{
		mc2RoadTrace.initialized = true;
		const char* env = getenv("MC2_ROAD_TRACE");
		mc2RoadTrace.wanted = (env && *env) ? atol(env) : 0;
		mc2RoadTrace.enabled = mc2RoadTrace.wanted > 0;
		if (mc2RoadTrace.enabled && !mc2RoadTrace.registered)
		{
			mc2RoadTrace.registered = true;
			atexit(MC2_RoadTraceAtExit);
		}
	}
	return mc2RoadTrace.enabled;
}

static bool MC2_RoadTraceResolve (void)
{
	if (mc2RoadTrace.resolved)
		return true;
	if (!GameMap)
		return false;
	const long side = Terrain::realVerticesMapSide * MAPCELL_DIM;
	long found = 0;
	for (long r = 0; r < side; r++)
	{
		for (long c = 0; c < side; c++)
		{
			if (GameMap->getOverlay(r, c) >= 3)
				continue;
			if (++found != mc2RoadTrace.wanted)
				continue;
			mc2RoadTrace.cellRow = r;
			mc2RoadTrace.cellCol = c;
			mc2RoadTrace.tileRow = r / MAPCELL_DIM;
			mc2RoadTrace.tileCol = c / MAPCELL_DIM;
			mc2RoadTrace.resolved = true;
			fprintf(stderr,
				"[road-trace] ARMED road=%ld cell=%ld,%ld tile=%ld,%ld; "
				"setup=0 is an explicit target-absent frame, not probe silence\n",
				mc2RoadTrace.wanted, r, c,
				mc2RoadTrace.tileRow, mc2RoadTrace.tileCol);
			fflush(stderr);
			return true;
		}
	}
	fprintf(stderr, "[road-trace] NO ROAD %ld in %ld x %ld cells; probe disabled\n",
		mc2RoadTrace.wanted, side, side);
	fflush(stderr);
	mc2RoadTrace.enabled = false;
	return false;
}

void MC2_RoadTraceBeginFrame (void)
{
	if (!MC2_RoadTraceOn() || !MC2_RoadTraceResolve())
		return;
	MC2_RoadTraceEmit();
	mc2RoadTrace.frame++;
	mc2RoadTrace.frameStarted = true;
	mc2RoadTrace.setupSeen = false;
	mc2RoadTrace.drawCalled = false;
	mc2RoadTrace.uvMode = 0;
	mc2RoadTrace.clipped1 = 0;
	mc2RoadTrace.clipped2 = 0;
	mc2RoadTrace.isCement = false;
	mc2RoadTrace.isAlpha = false;
	mc2RoadTrace.textureIndex = 0xffffffff;
	mc2RoadTrace.transitionType = 0;
	mc2RoadTrace.transitionOverlay = 0;
	mc2RoadTrace.transitionFound = false;
	mc2RoadTrace.terrainHandle = 0xffffffff;
	mc2RoadTrace.overlayHandle = 0xffffffff;
	mc2RoadTrace.screenX = -1.0f;
	mc2RoadTrace.screenY = -1.0f;
	mc2RoadTrace.overlayAddVertices = 0;
}

static bool MC2_RoadTraceIsTarget (const TerrainQuad* quad)
{
	if (!mc2RoadTrace.enabled || !mc2RoadTrace.resolved || !quad ||
		!quad->vertices[0])
		return false;
	const long row = quad->vertices[0]->posTile >> 16;
	const long col = quad->vertices[0]->posTile & 0x0000ffff;
	return row == mc2RoadTrace.tileRow && col == mc2RoadTrace.tileCol;
}

static void MC2_RoadTraceSetup (const TerrainQuad* quad)
{
	if (!MC2_RoadTraceIsTarget(quad))
		return;
	mc2RoadTrace.setupSeen = true;
	mc2RoadTrace.uvMode = quad->uvMode;
	if (quad->uvMode == BOTTOMRIGHT)
	{
		mc2RoadTrace.clipped1 = quad->vertices[0]->clipInfo + quad->vertices[1]->clipInfo + quad->vertices[2]->clipInfo;
		mc2RoadTrace.clipped2 = quad->vertices[0]->clipInfo + quad->vertices[2]->clipInfo + quad->vertices[3]->clipInfo;
	}
	else
	{
		mc2RoadTrace.clipped1 = quad->vertices[0]->clipInfo + quad->vertices[1]->clipInfo + quad->vertices[3]->clipInfo;
		mc2RoadTrace.clipped2 = quad->vertices[1]->clipInfo + quad->vertices[2]->clipInfo + quad->vertices[3]->clipInfo;
	}
	const DWORD low = quad->vertices[0]->pVertex->textureData & 0x0000ffff;
	mc2RoadTrace.isCement = Terrain::terrainTextures->isCement(low);
	mc2RoadTrace.isAlpha = Terrain::terrainTextures->isAlpha(low);
	mc2RoadTrace.textureIndex = low;
	mc2RoadTrace.transitionFound = MC2_GetTransitionInfo(
		low, mc2RoadTrace.transitionType, mc2RoadTrace.transitionOverlay);
	mc2RoadTrace.terrainHandle = quad->terrainHandle;
	mc2RoadTrace.overlayHandle = quad->overlayHandle;
	mc2RoadTrace.screenX = (quad->vertices[0]->px + quad->vertices[1]->px +
		quad->vertices[2]->px + quad->vertices[3]->px) * 0.25f;
	mc2RoadTrace.screenY = (quad->vertices[0]->py + quad->vertices[1]->py +
		quad->vertices[2]->py + quad->vertices[3]->py) * 0.25f;
}

static void MC2_RoadTraceDraw (const TerrainQuad* quad)
{
	if (MC2_RoadTraceIsTarget(quad))
		mc2RoadTrace.drawCalled = true;
}

static void MC2_RoadTraceOverlayAdd (const TerrainQuad* quad)
{
	if (MC2_RoadTraceIsTarget(quad))
		mc2RoadTrace.overlayAddVertices++;
}

// M8-B12, 2026-08-20. Every road quad reads isCement=1, so the cement gate is
// not what loses the road -- measured, both missions, zero cement0 quads. But
// quad.cpp assigns overlayHandle a REAL value at only two sites and assigns it
// 0xffffffff at eleven, and any of those eleven throws away the road on a quad
// that carries one.
//
// Rather than reason about which uvMode/clipped path a quad takes, make every
// discard site name itself with __LINE__ and count only the ones that landed on
// a quad the MAP says has a road. The line with the big number IS the answer.
static bool mc2RoadQuadHasRoad = false;

static void MC2_RoadDiscard (unsigned long line)
{
	mc2RoadCensus.culledAllQuads++;   // control: counted for EVERY quad
	if (!mc2RoadQuadHasRoad)
		return;
	for (unsigned long i = 0; i < mc2RoadCensus.discardSites; i++)
		if (mc2RoadCensus.discardSiteLine[i] == line) { mc2RoadCensus.discardBySite[i]++; return; }
	if (mc2RoadCensus.discardSites < 16)
	{
		mc2RoadCensus.discardSiteLine[mc2RoadCensus.discardSites] = line;
		mc2RoadCensus.discardBySite[mc2RoadCensus.discardSites] = 1;
		mc2RoadCensus.discardSites++;
	}
}

static bool MC2_RoadCensusOn (void)
{
	static const char* env = getenv("MC2_ROAD_CENSUS");
	static const bool on = env && *env && strcmp(env, "0") != 0;
	return on;
}

static void MC2_RoadCensusDump (void)
{
	fprintf(stderr,
		"[road] setup=%lu outOfMapSuppressed=%lu mapHasOverlay=%lu "
		"handleResolved=%lu overlayDrawn=%lu firstOverlayWord=0x%04lx useOverlayTexture=%d\n",
		mc2RoadCensus.setupCalls, mc2RoadCensus.outOfMapSuppressed,
		mc2RoadCensus.mapHasOverlay, mc2RoadCensus.handleResolved,
		mc2RoadCensus.overlayDrawn,
		(unsigned long)mc2RoadCensus.firstOverlayWord,
		useOverlayTexture ? 1 : 0);
	fprintf(stderr,
		"[road] suppressed by invalid-vertex count: 1=%lu 2=%lu 3=%lu 4=%lu (1-3 are BOUNDARY quads, 4 is genuinely off-map)\n",
		mc2RoadCensus.suppressedByBadCount[1], mc2RoadCensus.suppressedByBadCount[2],
		mc2RoadCensus.suppressedByBadCount[3], mc2RoadCensus.suppressedByBadCount[4]);
	fprintf(stderr,
		"[road] quads the MAP says carry a road, by the flags the draw branches on: "
		"cement0alpha0=%lu cement0alpha1=%lu cement1alpha0=%lu cement1alpha1=%lu (ONLY cement1 can draw the road)\n",
		mc2RoadCensus.overlayQuadByFlags[0], mc2RoadCensus.overlayQuadByFlags[1],
		mc2RoadCensus.overlayQuadByFlags[2], mc2RoadCensus.overlayQuadByFlags[3]);
	for (unsigned long i = 0; i < mc2RoadCensus.discardSites; i++)
		fprintf(stderr, "[road] ROAD DISCARDED at quad.cpp:%lu  x%lu\n",
				mc2RoadCensus.discardSiteLine[i], mc2RoadCensus.discardBySite[i]);
	{
		unsigned long roadCulled = 0;
		for (unsigned long i = 0; i < mc2RoadCensus.discardSites; i++)
			roadCulled += mc2RoadCensus.discardBySite[i];
		const unsigned long survivors =
			mc2RoadCensus.setupCalls - mc2RoadCensus.outOfMapSuppressed;
		fprintf(stderr,
			"[road] CONTROL culling rate: ALL quads %lu/%lu = %.1f%% ; ROAD quads %lu/%lu = %.1f%% (same rate means this is ordinary off-screen culling)\n",
			mc2RoadCensus.culledAllQuads, survivors,
			survivors ? (100.0 * mc2RoadCensus.culledAllQuads / survivors) : 0.0,
			roadCulled, mc2RoadCensus.mapHasOverlay,
			mc2RoadCensus.mapHasOverlay ? (100.0 * roadCulled / mc2RoadCensus.mapHasOverlay) : 0.0);
	}
	fflush(stderr);
}

static void MC2_RoadCensusArm (void)
{
	if (!mc2RoadCensus.registered)
	{
		mc2RoadCensus.registered = true;
		atexit(MC2_RoadCensusDump);
	}
}

void TerrainQuad::setupTextures (void)
{
	if (MC2_WaterCensusOn()) { MC2_WaterCensusArm(); mc2WaterCensus.setupCalls++; }
	mc2RoadQuadHasRoad = false;
	if (MC2_RoadCensusOn()) { MC2_RoadCensusArm(); mc2RoadCensus.setupCalls++; }
	// PORT: Expanded render windows include placeholder vertices outside the
	// authored map. TerrainColorMap rejects those coordinates with handle 0;
	// never submit the resulting quad as textured terrain.
	if ((vertices[0]->vertexNum < 0) || (vertices[1]->vertexNum < 0) ||
		(vertices[2]->vertexNum < 0) || (vertices[3]->vertexNum < 0))
	{
		terrainHandle = waterHandle = waterDetailHandle =
			terrainDetailHandle = overlayHandle = 0xffffffff;
		if (MC2_RoadCensusOn())
		{
			mc2RoadCensus.outOfMapSuppressed++;
			// M8-B12, 2026-08-20. The owner supplied a retail reference: the road
			// is CONTINUOUS there and MISSING CHUNKS here. That is not a texture
			// fault -- the tiles and the draw both check out -- it is whole quads
			// not being drawn.
			//
			// This guard is the port's own, and it drops a quad if ANY ONE of its
			// four vertices is an off-map placeholder. A quad with all four
			// invalid is genuinely outside the authored map and dropping it is
			// right. A quad with ONE, TWO or THREE invalid straddles the boundary
			// of the valid area -- it is a real quad, and dropping it removes a
			// piece of terrain and any road crossing it.
			//
			// So count the distribution rather than the total. A large 1..3
			// population is the defect; an all-4 population is the guard doing its
			// job. THE TOTAL ALONE CANNOT TELL THOSE APART, which is why the
			// earlier "44-65% of quads suppressed" figure proved nothing.
			int bad = 0;
			for (int v = 0; v < 4; v++)
				if (vertices[v]->vertexNum < 0) bad++;
			mc2RoadCensus.suppressedByBadCount[bad]++;
		}
		return;
	}

	// What the MAP says, read only for quads that SURVIVED the guard above.
	// Reading it earlier counted off-map placeholder vertices, whose
	// textureData is not 0xffff-high and therefore looked like overlays.
	if (MC2_RoadCensusOn() && vertices[0] && vertices[0]->pVertex)
	{
		const DWORD high = (vertices[0]->pVertex->textureData >> 16);
		if (high != 0xffff)
		{
			mc2RoadCensus.mapHasOverlay++;
			if (mc2RoadCensus.firstOverlayWord == 0xffffffff)
				mc2RoadCensus.firstOverlayWord = high;
			// M8-B12, 2026-08-20. THE ROAD OVERLAY IS ONLY EVER DRAWN INSIDE
			// THE isCement BRANCH. In the !isCement branch the very next thing
			// the code does is overlayHandle = 0xffffffff -- the road on that
			// quad is discarded before anything can draw it.
			//
			// So for a quad the MAP says carries a road, the pair (isCement,
			// isAlpha) decides whether that road can appear at all. Tally it.
			// isCement=0 on a road quad means the road cannot be drawn, and a
			// road that crosses a mixture of the two comes out IN PIECES --
			// which is exactly what the owner reported and what their retail
			// reference does not do.
			const DWORD low = (vertices[0]->pVertex->textureData & 0x0000ffff);
			const bool cementFlag = Terrain::terrainTextures->isCement(low);
			const bool alphaFlag  = Terrain::terrainTextures->isAlpha(low);
			mc2RoadCensus.overlayQuadByFlags[(cementFlag ? 2 : 0) + (alphaFlag ? 1 : 0)]++;
			mc2RoadQuadHasRoad = true;
		}
	}

	if (mineTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults\\mine_00",".tga");
		mineTextureHandle = mcTextureManager->loadTexture(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
	}
	
	if (blownTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults\\minescorch_00",".tga");
		blownTextureHandle = mcTextureManager->loadTexture(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
	}
	
 	if (!Terrain::terrainTextures2)
	{
		if (uvMode == BOTTOMRIGHT)
		{
			long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
			long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
						
			if (clipped1 || clipped2)
			{
				{
					terrainHandle = Terrain::terrainTextures->getTextureHandle((vertices[0]->pVertex->textureData & 0x0000ffff)); 
					DWORD terrainDetailData = Terrain::terrainTextures->setDetail(1,0);
					if (terrainDetailData != 0xfffffff)
						terrainDetailHandle = Terrain::terrainTextures->getTextureHandle(terrainDetailData);
					else
						terrainDetailHandle = 0xffffffff;
					overlayHandle = 0xffffffff;
						
					if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
				}
				
				//--------------------------------------------------------------------
				//Mine Information
				long rowCol = vertices[0]->posTile;
				long tileR = rowCol>>16;
				long tileC = rowCol & 0x0000ffff;
						
				if (GameMap)
				{
					mineResult.init();
					long cellPos = 0;
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							
							DWORD localResult = 0;
							if (GameMap->inBounds(actualCellRow, actualCellCol))
								localResult = GameMap->getMine(actualCellRow, actualCellCol);
								
							if (localResult == 1)
							{
								mcTextureManager->get_gosTextureHandle(mineTextureHandle);
								mcTextureManager->addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								mcTextureManager->addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								mcTextureManager->get_gosTextureHandle(blownTextureHandle);
								mcTextureManager->addTriangle(blownTextureHandle, MC2_DRAWALPHA);
								mcTextureManager->addTriangle(blownTextureHandle, MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
						}
					}
				}
			}
			else
			{
				terrainHandle = 0xffffffff; 
				waterHandle = 0xffffffff; 
				waterDetailHandle = 0xffffffff;
				terrainDetailHandle = 0xffffffff;
				overlayHandle = 0xffffffff;
				if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
			}
		}
		else
		{
			long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
			long clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
						
			if (clipped1 || clipped2)
			{
				{
					terrainHandle = Terrain::terrainTextures->getTextureHandle((vertices[0]->pVertex->textureData & 0x0000ffff)); 
					DWORD terrainDetailData = Terrain::terrainTextures->setDetail(1,0);
					if (terrainDetailData != 0xfffffff)
						terrainDetailHandle = Terrain::terrainTextures->getTextureHandle(terrainDetailData);
					else
						terrainDetailHandle = 0xffffffff;
					overlayHandle = 0xffffffff;
						
					if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
				}
				
				//--------------------------------------------------------------------
				//Mine Information
				long rowCol = vertices[0]->posTile;
				long tileR = rowCol>>16;
				long tileC = rowCol & 0x0000ffff;
						
				if (GameMap)
				{
					long cellPos = 0;
					mineResult.init();
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							
							DWORD localResult = 0;
							if (GameMap->inBounds(actualCellRow, actualCellCol))
								localResult = GameMap->getMine(actualCellRow, actualCellCol);
								
							if (localResult == 1)
							{
								mcTextureManager->get_gosTextureHandle(mineTextureHandle);
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								mcTextureManager->get_gosTextureHandle(blownTextureHandle);
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
						}
					}
				}
			}
			else
			{
				terrainHandle = 0xffffffff; 
				waterHandle = 0xffffffff; 
				waterDetailHandle = 0xffffffff;
				terrainDetailHandle = 0xffffffff;
				overlayHandle = 0xffffffff;
				if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
			}
		}
	}
	else		//New single bitmap on the terrain.
	{
		if (uvMode == BOTTOMRIGHT)
		{
			long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
			long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
						
			if (clipped1 || clipped2)
			{
				isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
				bool isAlpha = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff); 
				if (!isCement)
				{
					terrainHandle = Terrain::terrainTextures2->getTextureHandle(vertices[0],vertices[2],&uvData); 
					terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
					overlayHandle = 0xffffffff;
						
					if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					if (terrainDetailHandle != 0xffffffff)
					{
						mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
						mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
				}
				else		//This cement in the old Universe, pull the old texture and display it
				{
					if (isAlpha)		//This should be treated as an overlay.  Do so.  Draw overlay AFTER new terrain in same square!!
					{
						overlayHandle = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
						if (MC2_RoadCensusOn()) mc2RoadCensus.handleResolved++;
						terrainHandle = Terrain::terrainTextures2->getTextureHandle(vertices[1],vertices[3],&uvData);
						terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
						
						mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
						mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
						
						if (terrainDetailHandle != 0xffffffff)
						{
							mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
							mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
						}
						
						mcTextureManager->addTriangle(overlayHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
						mcTextureManager->addTriangle(overlayHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					}
					else	//Otherwise, its solid cement.  Save some draw cycles!!
					{
						terrainHandle = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
						terrainDetailHandle = 0xffffffff;		//Cement has NO detail!!
						overlayHandle = 0xffffffff;
						
						if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
						mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
						mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					}
				}
				
				//--------------------------------------------------------------------
				//Mine Information
				long rowCol = vertices[0]->posTile;
				long tileR = rowCol>>16;
				long tileC = rowCol & 0x0000ffff;
						
				if (GameMap)
				{
					long cellPos = 0;
					mineResult.init();
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							
							DWORD localResult = 0;
							if (GameMap->inBounds(actualCellRow, actualCellCol))
								localResult = GameMap->getMine(actualCellRow, actualCellCol);
								
							if (localResult == 1)
							{
								mcTextureManager->get_gosTextureHandle(mineTextureHandle);
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								mcTextureManager->get_gosTextureHandle(blownTextureHandle);
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
						}
					}
				}
			}
			else
			{
				overlayHandle = 0xffffffff;
				if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
				terrainHandle = 0xffffffff; 
				waterHandle = 0xffffffff; 
				waterDetailHandle = 0xffffffff;
				terrainDetailHandle = 0xffffffff;
			}
		}
		else
		{
			long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
			long clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
						
			if (clipped1 || clipped2)
			{
				isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
				bool isAlpha = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff); 
				if (!isCement)
				{
					terrainHandle = Terrain::terrainTextures2->getTextureHandle(vertices[1],vertices[3],&uvData); 
					terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
					overlayHandle = 0xffffffff;
					
					if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
				}
				else		//This cement in the old Universe, pull the old texture and display it
				{
					if (isAlpha)		//This should be treated as an overlay.  Do so.  Draw overlay AFTER new terrain in same square!!
					{
						overlayHandle = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
						if (MC2_RoadCensusOn()) mc2RoadCensus.handleResolved++;
						terrainHandle = Terrain::terrainTextures2->getTextureHandle(vertices[1],vertices[3],&uvData);
						terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
						
						mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
						mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
						mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
						mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
						mcTextureManager->addTriangle(overlayHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
						mcTextureManager->addTriangle(overlayHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					}
					else	//Otherwise, its solid cement.  Save some draw cycles!!
					{
						terrainHandle = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
						terrainDetailHandle = 0xffffffff;		//Cement has NO detail!!
						overlayHandle = 0xffffffff;
							
						if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
						mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
						mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					}
				}
				
				//--------------------------------------------------------------------
				//Mine Information
				long rowCol = vertices[0]->posTile;
				long tileR = rowCol>>16;
				long tileC = rowCol & 0x0000ffff;
						
				if (GameMap)
				{
					long cellPos = 0;
					mineResult.init();
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							
							DWORD localResult = 0;
							if (GameMap->inBounds(actualCellRow, actualCellCol))
								localResult = GameMap->getMine(actualCellRow, actualCellCol);
								
							if (localResult == 1)
							{
								mcTextureManager->get_gosTextureHandle(mineTextureHandle);
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								mcTextureManager->get_gosTextureHandle(blownTextureHandle);
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
						}
					}
				}
			}
			else
			{
				terrainHandle = 0xffffffff; 
				waterHandle = 0xffffffff; 
				waterDetailHandle = 0xffffffff;
				terrainDetailHandle = 0xffffffff;
				overlayHandle = 0xffffffff;
				if (MC2_RoadCensusOn()) MC2_RoadDiscard(__LINE__);
			}
		}
	}

	//-----------------------------------------
	// NEW(tm) water texture code here.
	if ((vertices[0]->pVertex->water & 1) ||
		(vertices[1]->pVertex->water & 1) ||
		(vertices[2]->pVertex->water & 1) ||
		(vertices[3]->pVertex->water & 1))
	{
		Stuff::Vector3D vertex3D(vertices[0]->vx,vertices[0]->vy,Terrain::waterElevation);
		Stuff::Vector4D screenPos;

		long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
		long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

		if (uvMode != BOTTOMRIGHT)
		{
			clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
			clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
		}

 		float negCos = Terrain::frameCos * -1.0;
		float ourCos = Terrain::frameCos;
		if (!(vertices[0]->calcThisFrame & 2))
		{
			if (clipped1 || clipped2)
			{
				if (vertices[0]->pVertex->water & 128)
				{
					vertices[0]->wAlpha = -Terrain::frameCosAlpha;
					ourCos = negCos;
				}
				else if (vertices[0]->pVertex->water & 64)
				{
					vertices[0]->wAlpha = Terrain::frameCosAlpha;
					ourCos = Terrain::frameCos;
				}
	
				vertex3D.z = ourCos + Terrain::waterElevation;
			
				bool clipData = false;
				clipData = eye->projectZ(vertex3D,screenPos); 
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
					vertices[0]->hazeFactor = 1.0f;
				}

				if (eye->usePerspective)
					vertices[0]->clipInfo = clipData;
				else
					vertices[0]->clipInfo = clipData;
		
				vertices[0]->wx = screenPos.x;
				vertices[0]->wy = screenPos.y;
				vertices[0]->wz = screenPos.z;
				vertices[0]->ww = screenPos.w;
	
				vertices[0]->calcThisFrame |= 2;

				if (clipData)
				{
					if (screenPos.z < leastZ)
					{
						leastZ = screenPos.z;
					}

					if (screenPos.z > mostZ)
					{
						mostZ = screenPos.z;
					}

					if (screenPos.w < leastW)
					{
						leastW = screenPos.w;
						leastWY = screenPos.y;
					}

					if (screenPos.w > mostW)
					{
						mostW = screenPos.w;
						mostWY = screenPos.y;
					}
				}
			}
		}
		
		if (!(vertices[1]->calcThisFrame & 2))
		{
			if (clipped1 || clipped2)
			{
				if (vertices[1]->pVertex->water & 128)
				{
					vertices[1]->wAlpha = -Terrain::frameCosAlpha;
					ourCos = negCos;
				}
				else if (vertices[1]->pVertex->water & 64)
				{
					vertices[1]->wAlpha = Terrain::frameCosAlpha;
					ourCos = Terrain::frameCos;
				}
	
				vertex3D.z = ourCos + Terrain::waterElevation;
				vertex3D.x = vertices[1]->vx;
				vertex3D.y = vertices[1]->vy;
				
				bool clipData = false;
				clipData = eye->projectZ(vertex3D,screenPos); 
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
					vertices[1]->hazeFactor = 1.0f;
				}

				if (eye->usePerspective)
					vertices[1]->clipInfo = clipData; //onScreen;
				else
					vertices[1]->clipInfo = clipData;
 
				vertices[1]->wx = screenPos.x;
				vertices[1]->wy = screenPos.y;
				vertices[1]->wz = screenPos.z;
				vertices[1]->ww = screenPos.w;
	
				vertices[1]->calcThisFrame |= 2;

				if (clipData)
				{
					if (screenPos.z < leastZ)
					{
						leastZ = screenPos.z;
					}

					if (screenPos.z > mostZ)
					{
						mostZ = screenPos.z;
					}

					if (screenPos.w < leastW)
					{
						leastW = screenPos.w;
						leastWY = screenPos.y;
					}

					if (screenPos.w > mostW)
					{
						mostW = screenPos.w;
						mostWY = screenPos.y;
					}
				}
			}
		}

		if (!(vertices[2]->calcThisFrame & 2))
		{
			if (clipped1 || clipped2)
			{
				if (vertices[2]->pVertex->water & 128)
				{
					vertices[2]->wAlpha = -Terrain::frameCosAlpha;
					ourCos = negCos;
				}
				else if (vertices[2]->pVertex->water & 64)
				{
					vertices[2]->wAlpha = Terrain::frameCosAlpha;
					ourCos = Terrain::frameCos;
				}
	
				vertex3D.z = ourCos + Terrain::waterElevation;
				vertex3D.x = vertices[2]->vx;
				vertex3D.y = vertices[2]->vy;
			
				bool clipData = false;
				clipData = eye->projectZ(vertex3D,screenPos); 
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
					vertices[2]->hazeFactor = 1.0f;
				}

				if (eye->usePerspective)
					vertices[2]->clipInfo = clipData; //onScreen;
				else
					vertices[2]->clipInfo = clipData;
					
				vertices[2]->wx = screenPos.x;
				vertices[2]->wy = screenPos.y;
				vertices[2]->wz = screenPos.z;
				vertices[2]->ww = screenPos.w;
	
				vertices[2]->calcThisFrame |= 2;

				if (clipData)
				{
					if (screenPos.z < leastZ)
					{
						leastZ = screenPos.z;
					}

					if (screenPos.z > mostZ)
					{
						mostZ = screenPos.z;
					}

					if (screenPos.w < leastW)
					{
						leastW = screenPos.w;
						leastWY = screenPos.y;
					}

					if (screenPos.w > mostW)
					{
						mostW = screenPos.w;
						mostWY = screenPos.y;
					}
				}
			}
		}

		if (!(vertices[3]->calcThisFrame & 2))
		{
			if (clipped1 || clipped2)
			{
				if (vertices[3]->pVertex->water & 128)
				{
					vertices[3]->wAlpha = -Terrain::frameCosAlpha;
					ourCos = negCos;
				}
				else if (vertices[3]->pVertex->water & 64)
				{
					vertices[3]->wAlpha = Terrain::frameCosAlpha;
					ourCos = Terrain::frameCos;
				}
	
				vertex3D.z = ourCos + Terrain::waterElevation;
				vertex3D.x = vertices[3]->vx;
				vertex3D.y = vertices[3]->vy;
				
				bool clipData = false;
				clipData = eye->projectZ(vertex3D,screenPos); 
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
					vertices[3]->hazeFactor = 1.0f;
				}

				if (eye->usePerspective)
					vertices[3]->clipInfo = clipData; //onScreen;
				else
					vertices[3]->clipInfo = clipData;
				 
				vertices[3]->wx = screenPos.x;
				vertices[3]->wy = screenPos.y;
				vertices[3]->wz = screenPos.z;
				vertices[3]->ww = screenPos.w;
	
				vertices[3]->calcThisFrame |= 2;

				if (clipData)
				{
					if (screenPos.z < leastZ)
					{
						leastZ = screenPos.z;
					}

					if (screenPos.z > mostZ)
					{
						mostZ = screenPos.z;
					}

					if (screenPos.w < leastW)
					{
						leastW = screenPos.w;
						leastWY = screenPos.y;
					}

					if (screenPos.w > mostW)
					{
						mostW = screenPos.w;
						mostWY = screenPos.y;
					}
				}
			}
		}

		if (clipped1 || clipped2)
		{
			if (!Terrain::terrainTextures2)
			{
				DWORD waterDetailData = Terrain::terrainTextures->setDetail(0,sprayFrame);
				waterHandle = Terrain::terrainTextures->getTextureHandle(MapData::WaterTXMData & 0x0000ffff);
				waterDetailHandle = Terrain::terrainTextures->getDetailHandle(waterDetailData & 0x0000ffff); 
			}
			else
			{
				waterHandle = Terrain::terrainTextures2->getWaterTextureHandle();
				waterDetailHandle = Terrain::terrainTextures2->getWaterDetailHandle(sprayFrame);
			}

			if (MC2_WaterCensusOn())
			{
				MC2_WaterCensusArm();
				mc2WaterCensus.branch++;
				if (waterHandle != 0xffffffff) mc2WaterCensus.handleValid++;
				else mc2WaterCensus.handleInvalid++;
				if (!mc2WaterCensus.sampled)
				{
					mc2WaterCensus.sampled = true;
					mc2WaterCensus.firstWater = waterHandle;
					mc2WaterCensus.firstDetail = waterDetailHandle;
				}
			}
			
			mcTextureManager->addTriangle(waterHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
			mcTextureManager->addTriangle(waterHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
			mcTextureManager->addTriangle(waterDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
			mcTextureManager->addTriangle(waterDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
		}
		else
		{
			waterHandle = 0xffffffff;
			waterDetailHandle = 0xffffffff;
		}
	}
	else
	{
		waterHandle = 0xffffffff;
		waterDetailHandle = 0xffffffff;
	}

	if (terrainHandle != 0xffffffff)
	{
		//-----------------------------------------------------
		// FOG time.
		float fogStart = eye->fogStart;
		float fogFull = eye->fogFull;

		//-----------------------------------------------------
		// Process Vertex 0 if not already done
		if (!(vertices[0]->calcThisFrame & 1)) 
		{
			DWORD specR=0, specG=0, specB=0;
			DWORD lightr=0xff,lightg=0xff,lightb=0xff;
			if (Environment.Renderer != 3)
			{
				//------------------------------------------------------------
				float lightIntensity = vertices[0]->pVertex->vertexNormal * eye->lightDirection;

				unsigned char shadow = vertices[0]->pVertex->shadow;
				if (shadow && lightIntensity > 0.2f)
				{
					lightIntensity = 0.2f;
				}

				lightr = eye->getLightRed(lightIntensity);
				lightg = eye->getLightGreen(lightIntensity);
				lightb = eye->getLightBlue(lightIntensity);

				if (BaseVertexColor)
				{
					lightr += ((BaseVertexColor>>16) & 0x000000ff);
					if (lightr > 0xff)
						lightr = 0xff;

					lightg += ((BaseVertexColor>>8) & 0x000000ff);
					if (lightg > 0xff)
						lightg = 0xff;

					lightb += (BaseVertexColor & 0x000000ff);
					if (lightb > 0xff)
						lightb = 0xff;
				}

				if (rainLightLevel < 1.0f)
				{
					lightr = (float)lightr * rainLightLevel;
					lightb = (float)lightb * rainLightLevel;
					lightg = (float)lightg * rainLightLevel;
				}

				if (lighteningLevel > 0x0)
				{
					specR = specG = specB = lighteningLevel;
				}

				vertices[0]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);

				//First two light are already factored into the above equations!
				for (long i=2;i<eye->getNumTerrainLights();i++)
				{
					TG_LightPtr thisLight = eye->getTerrainLight(i);
					if (thisLight)
					{
	//					if (useShadows)
						{
							if ((thisLight->lightType == TG_LIGHT_POINT) || 
								(thisLight->lightType == TG_LIGHT_SPOT) ||
								(thisLight->lightType == TG_LIGHT_TERRAIN))
							{
								Stuff::Point3D vertexToLight;

								vertexToLight.x = vertices[0]->vx;
								vertexToLight.y = vertices[0]->vy;
								vertexToLight.z = vertices[0]->pVertex->elevation;

								vertexToLight -= thisLight->position;

								float length = vertexToLight.GetApproximateLength();
								float falloff = 1.0f;

								const bool mc2VertexLit = thisLight->GetFalloff(length, falloff);
								MC2_TerrainLightTally(thisLight, mc2VertexLit, falloff);
								if (mc2VertexLit)
								{
									float red,green,blue;
									red = float((thisLight->GetaRGB()>>16) & 0x000000ff) * falloff;
									green = float((thisLight->GetaRGB()>>8) & 0x000000ff) * falloff;
									blue = float((thisLight->GetaRGB()) & 0x000000ff) * falloff;

									specR += (DWORD)red;
									specG += (DWORD)green;
									specB += (DWORD)blue;

									if (specR > 255)
										specR = 255;

									if (specG > 255)
										specG = 255;

									if (specB > 255)
										specB = 255;
								}
							}
						}
					}
				}
			}
			
			vertices[0]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);
			vertices[0]->fogRGB = (0xff<<24) + (specR<<16) + (specG << 8) + (specB);
				
			//Fog
			DWORD fogResult = 0xff;
			if (!(vertices[0]->calcThisFrame & 1)) 
			{
				if (useFog)
				{
					if (vertices[0]->pVertex->elevation < fogStart)
					{
						float fogFactor = fogStart - vertices[0]->pVertex->elevation;
						if (fogFactor < 0.0)
						{
							fogResult = 0xff;
						}
						else
						{
							fogFactor /= (fogStart - fogFull);
							if (fogFactor <= 1.0)
							{
								fogFactor *= fogFactor;
								fogFactor = 1.0 - fogFactor;
								fogFactor *= 256.0;
							}
							else
							{
								fogFactor = 256.0;
							}
							
							fogResult = float2long(fogFactor);
						}
					}
					else
					{
						fogResult = 0xff;
					}
					
					vertices[0]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
				}
			}

			//-------------------
			// Distance FOG now.
			if (vertices[0]->hazeFactor != 0.0f)
			{
				float fogFactor = 1.0 - vertices[0]->hazeFactor;
				DWORD distFog = float2long(fogFactor * 255.0f);
				
				if (distFog < fogResult)
				   fogResult = distFog;
				
				vertices[0]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
			}

			vertices[0]->calcThisFrame |= 1;
		}

		//-----------------------------------------------------
		// Process Vertex 1 if not already done
		if (!(vertices[1]->calcThisFrame & 1))
		{
			DWORD specR=0, specG=0, specB=0;
			DWORD lightr=0xff,lightg=0xff,lightb=0xff;
			if (Environment.Renderer != 3)
			{
				float lightIntensity = vertices[1]->pVertex->vertexNormal * eye->lightDirection;

				unsigned char shadow = vertices[1]->pVertex->shadow;
				if (shadow && lightIntensity > 0.2f)
				{
					lightIntensity = 0.2f;
				}

				lightr = eye->getLightRed(lightIntensity);
				lightg = eye->getLightGreen(lightIntensity);
				lightb = eye->getLightBlue(lightIntensity);

				if (BaseVertexColor)
				{
					lightr += ((BaseVertexColor>>16) & 0x000000ff);
					if (lightr > 0xff)
						lightr = 0xff;

					lightg += ((BaseVertexColor>>8) & 0x000000ff);
					if (lightg > 0xff)
						lightg = 0xff;

					lightb += (BaseVertexColor & 0x000000ff);
					if (lightb > 0xff)
						lightb = 0xff;
				}
				if (rainLightLevel < 1.0f)
				{
					lightr = (float)lightr * rainLightLevel;
					lightb = (float)lightb * rainLightLevel;
					lightg = (float)lightg * rainLightLevel;
				}

				if (lighteningLevel > 0x0)
				{
					specR = specG = specB = lighteningLevel;
				}
				vertices[1]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);

				//First two light are already factored into the above equations!
				for (long i=2;i<eye->getNumTerrainLights();i++)
				{
					TG_LightPtr thisLight = eye->getTerrainLight(i);
					if (thisLight)
					{
	//					if (useShadows)
						{
							if ((thisLight->lightType == TG_LIGHT_POINT) || 
								(thisLight->lightType == TG_LIGHT_SPOT) ||
								(thisLight->lightType == TG_LIGHT_TERRAIN))
							{
								Stuff::Point3D vertexToLight;
								vertexToLight.x = vertices[1]->vx;
								vertexToLight.y = vertices[1]->vy;
								vertexToLight.z = vertices[1]->pVertex->elevation;

								vertexToLight -= thisLight->position;

								float length = vertexToLight.GetApproximateLength();
								float falloff = 1.0f;

								const bool mc2VertexLit = thisLight->GetFalloff(length, falloff);
								MC2_TerrainLightTally(thisLight, mc2VertexLit, falloff);
								if (mc2VertexLit)
								{
									float red,green,blue;

									red = float((thisLight->GetaRGB()>>16) & 0x000000ff) * falloff;
									green = float((thisLight->GetaRGB()>>8) & 0x000000ff) * falloff;
									blue = float((thisLight->GetaRGB()) & 0x000000ff) * falloff;

									specR += (DWORD)red;
									specG += (DWORD)green;
									specB += (DWORD)blue;

									if (specR > 255)
										specR = 255;

									if (specG > 255)
										specG = 255;

									if (specB > 255)
										specB = 255;
								}
							}
						}
					}
				}
			}
			
			vertices[1]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);
			vertices[1]->fogRGB = (0xff<<24) + (specR<<16) + (specG << 8) + (specB);

			//Fog
			DWORD fogResult = 0xff;
			if (Environment.Renderer != 3)
			{
				if (useFog)
				{
					if (vertices[1]->pVertex->elevation < fogStart)
					{
					   float fogFactor = fogStart - vertices[1]->pVertex->elevation;
					   if (fogFactor < 0.0)
					   {
						   fogResult = 0xff;
					   }
					   else
					   {
						   fogFactor /= (fogStart - fogFull);
						   if (fogFactor <= 1.0)
						   {
							   fogFactor *= fogFactor;
							   fogFactor = 1.0 - fogFactor;
							   fogFactor *= 256.0;
						   }
						   else
						   {
							   fogFactor = 256.0;
						   }
						   
						   fogResult = float2long(fogFactor);
					   }
					}
					else
					{
					   fogResult = 0xff;
					}
					
					vertices[1]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
				}
			}

			//-------------------
			// Distance FOG now.
			if (vertices[1]->hazeFactor != 0.0f)
			{
				float fogFactor = 1.0 - vertices[1]->hazeFactor;
				DWORD distFog = float2long(fogFactor * 255.0);
				
				if (distFog < fogResult)
				   fogResult = distFog;
				   
				vertices[1]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
			}

			vertices[1]->calcThisFrame |= 1;
		}

		//-----------------------------------------------------
		// Process Vertex 2 if not already done
		if (!(vertices[2]->calcThisFrame & 1))
		{
			DWORD specR=0, specG=0, specB=0;
			DWORD lightr=0xff,lightg=0xff,lightb=0xff;
			if (Environment.Renderer != 3)
			{
				float lightIntensity = vertices[2]->pVertex->vertexNormal * eye->lightDirection;
	
				unsigned char shadow = vertices[2]->pVertex->shadow;
				if (shadow && lightIntensity > 0.2f)
				{
					lightIntensity = 0.2f;
				}
							  
				lightr = eye->getLightRed(lightIntensity);
				lightg = eye->getLightGreen(lightIntensity);
				lightb = eye->getLightBlue(lightIntensity);
					
				if (BaseVertexColor)
				{
					lightr += ((BaseVertexColor>>16) & 0x000000ff);
					if (lightr > 0xff)
						lightr = 0xff;
						
					lightg += ((BaseVertexColor>>8) & 0x000000ff);
					if (lightg > 0xff)
						lightg = 0xff;
						
					lightb += (BaseVertexColor & 0x000000ff);
					if (lightb > 0xff)
						lightb = 0xff;
				}
				if (rainLightLevel < 1.0f)
				{
					lightr = (float)lightr * rainLightLevel;
					lightb = (float)lightb * rainLightLevel;
					lightg = (float)lightg * rainLightLevel;
				}

				if (lighteningLevel > 0x0)
				{
					specR = specG = specB = lighteningLevel;
				}
				vertices[2]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);

				//First two light are already factored into the above equations!
				for (long i=2;i<eye->getNumTerrainLights();i++)
				{
					TG_LightPtr thisLight = eye->getTerrainLight(i);
					if (thisLight)
					{
	//					if (useShadows)
						{
							if ((thisLight->lightType == TG_LIGHT_POINT) || 
								(thisLight->lightType == TG_LIGHT_SPOT) ||
								(thisLight->lightType == TG_LIGHT_TERRAIN))
							{
								Stuff::Point3D vertexToLight;
								vertexToLight.x = vertices[2]->vx;
								vertexToLight.y = vertices[2]->vy;
								vertexToLight.z = vertices[2]->pVertex->elevation;

								vertexToLight -= thisLight->position;

								float length = vertexToLight.GetApproximateLength();
								float falloff = 1.0f;

								const bool mc2VertexLit = thisLight->GetFalloff(length, falloff);
								MC2_TerrainLightTally(thisLight, mc2VertexLit, falloff);
								if (mc2VertexLit)
								{
									float red,green,blue;

									red = float((thisLight->GetaRGB()>>16) & 0x000000ff) * falloff;
									green = float((thisLight->GetaRGB()>>8) & 0x000000ff) * falloff;
									blue = float((thisLight->GetaRGB()) & 0x000000ff) * falloff;

									specR += (DWORD)red;
									specG += (DWORD)green;
									specB += (DWORD)blue;

									if (specR > 255)
										specR = 255;

									if (specG > 255)
										specG = 255;

									if (specB > 255)
										specB = 255;
								}
							}
						}
					}
				}
			}
			
			vertices[2]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);
			vertices[2]->fogRGB = (0xff<<24) + (specR<<16) + (specG << 8) + (specB);

			//Fog
			DWORD fogResult = 0xff;
			if (Environment.Renderer != 3)
			{
				if (useFog)
				{
				   if (vertices[2]->pVertex->elevation < fogStart)
				   {
					   float fogFactor = fogStart - vertices[2]->pVertex->elevation;
					   if ((fogFactor < 0.0) || (0.0 == (fogStart - fogFull)))
					   {
						   fogResult = 0xff;
					   }
					   else
					   {
						   fogFactor /= (fogStart - fogFull);
						   if (fogFactor <= 1.0)
						   {
							   fogFactor *= fogFactor;
							   fogFactor = 1.0 - fogFactor;
							   fogFactor *= 256.0;
						   }
						   else
						   {
							   fogFactor = 256.0;
						   }
						   
						   fogResult = float2long(fogFactor);
					   }
				   }
				   else
				   {
					   fogResult = 0xff;
				   }
					
					vertices[2]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
				}
			}

			//-------------------
			// Distance FOG now.
			if (vertices[2]->hazeFactor != 0.0f)
			{
				float fogFactor = 1.0 - vertices[2]->hazeFactor;
				DWORD distFog = float2long(fogFactor * 255.0f);
				
				if (distFog < fogResult)
				   fogResult = distFog;
				   
				vertices[2]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
			}

			vertices[2]->calcThisFrame |= 1;
		}

		//-----------------------------------------------------
		// Process Vertex 3 if not already done
		if (!(vertices[3]->calcThisFrame & 1))
		{
			DWORD specR=0, specG=0, specB=0;
			DWORD lightr=0xff,lightg=0xff,lightb=0xff;
			if (Environment.Renderer != 3)
			{
				float lightIntensity = vertices[3]->pVertex->vertexNormal * eye->lightDirection;
	
				unsigned char shadow = vertices[3]->pVertex->shadow;
				if (shadow && lightIntensity > 0.2f)
				{
					lightIntensity = 0.2f;
				}
							  
				lightr = eye->getLightRed(lightIntensity);
				lightg = eye->getLightGreen(lightIntensity);
				lightb = eye->getLightBlue(lightIntensity);
					
				if (BaseVertexColor)
				{
					lightr += ((BaseVertexColor>>16) & 0x000000ff);
					if (lightr > 0xff)
						lightr = 0xff;
						
					lightg += ((BaseVertexColor>>8) & 0x000000ff);
					if (lightg > 0xff)
						lightg = 0xff;
						
					lightb += (BaseVertexColor & 0x000000ff);
					if (lightb > 0xff)
						lightb = 0xff;
				}
				if (rainLightLevel < 1.0f)
				{
					lightr = (float)lightr * rainLightLevel;
					lightb = (float)lightb * rainLightLevel;
					lightg = (float)lightg * rainLightLevel;
				}

				if (lighteningLevel > 0x0)
				{
					specR = specG = specB = lighteningLevel;
				}
				vertices[3]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);

				//First two light are already factored into the above equations!
				for (long i=2;i<eye->getNumTerrainLights();i++)
				{
					TG_LightPtr thisLight = eye->getTerrainLight(i);
					if (thisLight)
					{
	//					if (useShadows)
						{
							if ((thisLight->lightType == TG_LIGHT_POINT) || 
								(thisLight->lightType == TG_LIGHT_SPOT) ||
								(thisLight->lightType == TG_LIGHT_TERRAIN))
							{
								Stuff::Point3D vertexToLight;
								vertexToLight.x = vertices[3]->vx;
								vertexToLight.y = vertices[3]->vy;
								vertexToLight.z = vertices[3]->pVertex->elevation;

								vertexToLight -= thisLight->position;

								float length = vertexToLight.GetApproximateLength();
								float falloff = 1.0f;

								const bool mc2VertexLit = thisLight->GetFalloff(length, falloff);
								MC2_TerrainLightTally(thisLight, mc2VertexLit, falloff);
								if (mc2VertexLit)
								{
									float red,green,blue;

									red = float((thisLight->GetaRGB()>>16) & 0x000000ff) * falloff;
									green = float((thisLight->GetaRGB()>>8) & 0x000000ff) * falloff;
									blue = float((thisLight->GetaRGB()) & 0x000000ff) * falloff;

									specR += (DWORD)red;
									specG += (DWORD)green;
									specB += (DWORD)blue;

									if (specR > 255)
										specR = 255;

									if (specG > 255)
										specG = 255;

									if (specB > 255)
										specB = 255;
								}
							}
						}
					}
				}
			}
			
			vertices[3]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);
			vertices[3]->fogRGB = (0xff<<24) + (specR<<16) + (specG << 8) + (specB);
			
			//Fog
			DWORD fogResult = 0xff;
			if (Environment.Renderer != 3)
			{
				if (useFog)
				{
					if (vertices[3]->pVertex->elevation < fogStart)
					{
					   float fogFactor = fogStart - vertices[3]->pVertex->elevation;
					   if (fogFactor < 0.0)
					   {
						   fogResult = 0xff;
					   }
					   else
					   {
						   fogFactor /= (fogStart - fogFull);
						   if (fogFactor <= 1.0)
						   {
							   fogFactor *= fogFactor;
							   fogFactor = 1.0 - fogFactor;
							   fogFactor *= 256.0;
						   }
						   else
						   {
							   fogFactor = 256.0;
						   }
						   
						   fogResult = float2long(fogFactor);
					   }
					}
					else
					{
					   fogResult = 0xff;
					}
				}
			}

			//-------------------
			// Distance FOG now.
			if (vertices[3]->hazeFactor != 0.0f)
			{
				float fogFactor = 1.0 - vertices[3]->hazeFactor;
				DWORD distFog = float2long(fogFactor * 255.0f);
				
				if (distFog < fogResult)
				   fogResult = distFog;
				   
				vertices[3]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
			}

			vertices[3]->calcThisFrame |= 1;
		}
	}
	MC2_RoadTraceSetup(this);
}

#define TERRAIN_DEPTH_FUDGE		0.001f
//---------------------------------------------------------------------------
void TerrainQuad::draw (void)
{
	MC2_RoadTraceDraw(this);
	if (terrainHandle != 0xffffffff)
	{
		numTerrainFaces++;

		//---------------------------------------
		// GOS 3D draw Calls now!

		gos_VERTEX gVertex[3];

		float minU = 0.00f;
		float maxU = 0.9999f;
								
		float minV = 0.00f;
		float maxV = 0.9999f;
		
		float oldminU = 0.0078125f;
		float oldmaxU = 0.9921875f;

		float oldminV = 0.0078125f;
		float oldmaxV = 0.9921875f;
 		
		if (Terrain::terrainTextures2 && !(overlayHandle == 0xffffffff && isCement))
		{
			minU = uvData.minU;
			minV = uvData.minV;
			maxU = uvData.maxU;
			maxV = uvData.maxV;
		}

		Stuff::Point3D camPosition;
		camPosition = *TG_Shape::cameraOrigin;

		if (uvMode == BOTTOMRIGHT)
		{
			//--------------------------
			// Top Triangle
			DWORD lightRGB0 = vertices[0]->lightRGB; 
			DWORD lightRGB1 = vertices[1]->lightRGB; 
			DWORD lightRGB2 = vertices[2]->lightRGB; 
			if (Terrain::terrainTextures2 && (!Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff)
											|| Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff)))
				lightRGB0 = lightRGB1 = lightRGB2 = 0xffffffff;

			lightRGB0 = vertices[0]->pVertex->selected ? SELECTION_COLOR : lightRGB0;
			lightRGB1 = vertices[1]->pVertex->selected ? SELECTION_COLOR : lightRGB1;
			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

			gVertex[0].x		= vertices[0]->px;
			gVertex[0].y		= vertices[0]->py;
			gVertex[0].z		= vertices[0]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->pw;
			gVertex[0].u		= minU;
			gVertex[0].v		= minV;
			gVertex[0].argb		= lightRGB0;
			gVertex[0].frgb		= vertices[0]->fogRGB;

			gVertex[1].x		= vertices[1]->px;
			gVertex[1].y		= vertices[1]->py;
			gVertex[1].z		= vertices[1]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= minV;
			gVertex[1].argb		= lightRGB1;
			gVertex[1].frgb		= vertices[1]->fogRGB;

			gVertex[2].x		= vertices[2]->px;
			gVertex[2].y		= vertices[2]->py;
			gVertex[2].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[2]->pw;
			gVertex[2].u		= maxU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB2;
			gVertex[2].frgb		= vertices[2]->fogRGB;
			
			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					if ((terrainDetailHandle == 0xffffffff) && (overlayHandle == 0xffffffff) && isCement)
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					else
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
																										 
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff) &&
						(MC2_RoadCensusOn() ? (++mc2RoadCensus.overlayDrawn, true) : true))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldminV;
						oVertex[2].u		= oldmaxU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[1]->lightRGB;
						oVertex[2].argb		= vertices[2]->lightRGB;
						
						MC2_RoadTraceOverlayAdd(this);
						mcTextureManager->addVertices(overlayHandle,oVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					}
					
					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
						float oneOverTf		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTf;
		
						sVertex[1].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTf;
		
						sVertex[2].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTf;
						
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}

						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}

						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);	
					}
				}
			}

			//--------------------------
			//Bottom Triangle
			//
			// gVertex[0] same as above gVertex[0].
			// gVertex[1] is same as above gVertex[2].
			// gVertex[2] is calced from vertex[3].
			DWORD lightRGB3 = vertices[3]->lightRGB;
			if (Terrain::terrainTextures2 && (!Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff)
											|| Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff)))
				lightRGB3 = 0xffffffff;

			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

			gVertex[1].x		= gVertex[2].x;	
			gVertex[1].y		= gVertex[2].y;	
			gVertex[1].z		= gVertex[2].z;	
			gVertex[1].rhw		= gVertex[2].rhw;
			gVertex[1].u		= gVertex[2].u;
			gVertex[1].v		= gVertex[2].v;	
			gVertex[1].argb		= gVertex[2].argb;
			gVertex[1].frgb		= gVertex[2].frgb;

			gVertex[2].x		= vertices[3]->px;
			gVertex[2].y		= vertices[3]->py;
			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->pw;
			gVertex[2].u		= minU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB3;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			
			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					if ((terrainDetailHandle == 0xffffffff) && (overlayHandle == 0xffffffff) && isCement)
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					else
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
 
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff) &&
						(MC2_RoadCensusOn() ? (++mc2RoadCensus.overlayDrawn, true) : true))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldmaxV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;
						
						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[2]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						MC2_RoadTraceOverlayAdd(this);
						mcTextureManager->addVertices(overlayHandle,oVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					}
					
					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
 						float oneOverTF		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF;
		
						sVertex[1].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}

						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}

						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
				}
			}
		}
		else if (uvMode == BOTTOMLEFT)
		{
			//------------------------------
			// Top Triangle.
			DWORD lightRGB0 = vertices[0]->lightRGB;
			DWORD lightRGB1 = vertices[1]->lightRGB;
			DWORD lightRGB3 = vertices[3]->lightRGB;
			if (Terrain::terrainTextures2 && (!Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff)
											|| Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff)))
				lightRGB0 = lightRGB1 = lightRGB3 = 0xffffffff;

			lightRGB0 = vertices[0]->pVertex->selected ? SELECTION_COLOR : lightRGB0;
			lightRGB1 = vertices[1]->pVertex->selected ? SELECTION_COLOR : lightRGB1;
			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

			gVertex[0].x		= vertices[0]->px;
			gVertex[0].y		= vertices[0]->py;
			gVertex[0].z		= vertices[0]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->pw;
			gVertex[0].u		= minU;
			gVertex[0].v		= minV;
			gVertex[0].argb		= lightRGB0;
			gVertex[0].frgb		= vertices[0]->fogRGB;
		
			gVertex[1].x		= vertices[1]->px;
			gVertex[1].y		= vertices[1]->py;
			gVertex[1].z		= vertices[1]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= minV;
			gVertex[1].argb		= lightRGB1;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			
			gVertex[2].x		= vertices[3]->px;
			gVertex[2].y		= vertices[3]->py;
			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->pw;
			gVertex[2].u		= minU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB3;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			
			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					if ((terrainDetailHandle == 0xffffffff) && (overlayHandle == 0xffffffff) && isCement)
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					else
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
 
					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
 						float oneOverTF		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF;
		
						sVertex[1].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}
						
						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
					
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff) &&
						(MC2_RoadCensusOn() ? (++mc2RoadCensus.overlayDrawn, true) : true))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldminV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;
						
						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[1]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						MC2_RoadTraceOverlayAdd(this);
						mcTextureManager->addVertices(overlayHandle,oVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					}
				}
			}

			//---------------------------------------
			// Bottom Triangle.
			// gVertex[0] is same as above gVertex[1]
			// gVertex[1] is new and calced from vertex[2].
			// gVertex[2] is same as above.
			DWORD lightRGB2 = vertices[2]->lightRGB;
			if (Terrain::terrainTextures2 && (!Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff)
											|| Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff)))
				lightRGB2 = 0xffffffff;

			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

			gVertex[0].x		= gVertex[1].x;	
			gVertex[0].y		= gVertex[1].y;	
			gVertex[0].z		= gVertex[1].z;	
			gVertex[0].rhw		= gVertex[1].rhw;	
			gVertex[0].u		= gVertex[1].u;	
			gVertex[0].v		= gVertex[1].v;	
			gVertex[0].argb		= gVertex[1].argb;
			gVertex[0].frgb		= gVertex[1].frgb;

			gVertex[1].x		= vertices[2]->px;
			gVertex[1].y		= vertices[2]->py;
			gVertex[1].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[2]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= maxV;
			gVertex[1].argb		= lightRGB2;
			gVertex[1].frgb		= vertices[2]->fogRGB;
		
			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					if ((terrainDetailHandle == 0xffffffff) && (overlayHandle == 0xffffffff) && isCement)
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					else
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
 
					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
 						float oneOverTf		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTf;
		
						sVertex[1].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTf;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTf ;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTf ;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}
						
						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
					
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff) &&
						(MC2_RoadCensusOn() ? (++mc2RoadCensus.overlayDrawn, true) : true))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldmaxU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldmaxV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;
						
						//Light the overlays!!
						oVertex[0].argb		= vertices[1]->lightRGB;
						oVertex[1].argb		= vertices[2]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						MC2_RoadTraceOverlayAdd(this);
						mcTextureManager->addVertices(overlayHandle,oVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS);
					}
				}
			}
		}
	}

#ifdef _DEBUG
	if (selected )
	{
		drawLine();
		selected = FALSE;		
	}
#endif

}

extern float elevationAnimation;
#define SKY_FUDGE			(1.0f / 60000.0f)
#define WATER_ALPHA			0x1fffffff
extern float cloudScrollX;
extern float cloudScrollY;

//---------------------------------------------------------------------------
void TerrainQuad::drawWater (void)
{
	if (MC2_WaterCensusOn())
	{
		MC2_WaterCensusArm();
		mc2WaterCensus.drawCalls++;
		if (waterHandle != 0xffffffff) mc2WaterCensus.drawTextured++;
		else mc2WaterCensus.drawSkipped++;
	}
	float cloudOffsetX = cos(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollX) * 0.1f;
	float cloudOffsetY = sin(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollY) * 0.1f;
	
	float sprayOffsetX = cloudScrollX * 10.0f;
	float sprayOffsetY = cloudScrollY * 10.0f;

	//Gotta be able to run the untextured maps!!!
	float oneOverWaterTF = 1.0f / 64.0f;
	float oneOverTF = 1.0f / 64.0f;

	if (Terrain::terrainTextures2)
	{
		oneOverWaterTF = (Terrain::terrainTextures2->getWaterDetailTilingFactor() / Terrain::worldUnitsMapSide);
		oneOverTF = (Terrain::terrainTextures2->getWaterTextureTilingFactor() / Terrain::worldUnitsMapSide);
	}

	if (waterHandle != 0xffffffff)
	{
		numTerrainFaces++;

		//---------------------------------------
		// GOS 3D draw Calls now!

		float minU = 0.0f;
		float maxU = 0.9999999f;

		float minV = 0.0f;
		float maxV = 0.9999999f;

		//-----------------------------------------------------
		// FOG time.  Set Render state to FOG on!
		if (useFog)
		{
			DWORD fogColor = eye->fogColor;
			gos_SetRenderState( gos_State_Fog, (intptr_t)&fogColor);
		}
		else
		{
			gos_SetRenderState( gos_State_Fog, 0);
		}

		gos_VERTEX gVertex[3];
		if (uvMode == BOTTOMRIGHT)
		{
			//--------------------------
			// Top Triangle
			gVertex[0].x		= vertices[0]->wx;
			gVertex[0].y		= vertices[0]->wy;
			gVertex[0].z		= vertices[0]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->ww;
			gVertex[0].u		= minU + cloudOffsetX;
			gVertex[0].v		= minV + cloudOffsetY;
			gVertex[0].argb		= vertices[0]->pVertex->selected ? SELECTION_COLOR : vertices[0]->lightRGB;
			gVertex[0].frgb		= vertices[0]->fogRGB;

			gVertex[1].x		= vertices[1]->wx;
			gVertex[1].y		= vertices[1]->wy;
			gVertex[1].z		= vertices[1]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;
			gVertex[1].v		= minV + cloudOffsetY;
			gVertex[1].argb		= vertices[1]->pVertex->selected ? SELECTION_COLOR : vertices[1]->lightRGB;
			gVertex[1].frgb		= vertices[1]->fogRGB;

			gVertex[2].x		= vertices[2]->wx;
			gVertex[2].y		= vertices[2]->wy;
			gVertex[2].z		= vertices[2]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[2]->ww;
			gVertex[2].u		= maxU + cloudOffsetX;
			gVertex[2].v		= maxV + cloudOffsetY;
			gVertex[2].argb		= vertices[2]->pVertex->selected ? SELECTION_COLOR : vertices[2]->lightRGB;
			gVertex[2].frgb		= vertices[2]->fogRGB;

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[1].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY; 

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
				}
 			}
   
			//--------------------------
			//Bottom Triangle
			//
			// gVertex[0] same as above gVertex[0].
			// gVertex[1] is same as above gVertex[2].
			// gVertex[2] is calced from vertex[3].
			gVertex[1].x		= gVertex[2].x;	
			gVertex[1].y		= gVertex[2].y;	
			gVertex[1].z		= gVertex[2].z;	
			gVertex[1].rhw		= gVertex[2].rhw;
			gVertex[1].u		= gVertex[2].u;
			gVertex[1].v		= gVertex[2].v;	
			gVertex[1].argb		= gVertex[2].argb;
			gVertex[1].frgb		= gVertex[2].frgb;

			gVertex[2].x		= vertices[3]->wx;
			gVertex[2].y		= vertices[3]->wy;
			gVertex[2].z		= vertices[3]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->ww;
			gVertex[2].u		= minU + cloudOffsetX;
			gVertex[2].v		= maxV + cloudOffsetY;
			gVertex[2].argb		= vertices[3]->pVertex->selected ? SELECTION_COLOR : vertices[3]->lightRGB;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			
			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[1].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY; 

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
	
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
				}
			}
		}
		else if (uvMode == BOTTOMLEFT)
		{
			//------------------------------
			// Top Triangle.
			gVertex[0].x		= vertices[0]->wx;
			gVertex[0].y		= vertices[0]->wy;
			gVertex[0].z		= vertices[0]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->ww;
			gVertex[0].u		= minU + cloudOffsetX;;
			gVertex[0].v		= minV + cloudOffsetY;;
			gVertex[0].argb		= vertices[0]->pVertex->selected ? SELECTION_COLOR : vertices[0]->lightRGB;
			gVertex[0].frgb		= vertices[0]->fogRGB;
		
			gVertex[1].x		= vertices[1]->wx;
			gVertex[1].y		= vertices[1]->wy;
			gVertex[1].z		= vertices[1]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;;
			gVertex[1].v		= minV + cloudOffsetY;;
			gVertex[1].argb		= vertices[1]->pVertex->selected ? SELECTION_COLOR : vertices[1]->lightRGB;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			
			gVertex[2].x		= vertices[3]->wx;
			gVertex[2].y		= vertices[3]->wy;
			gVertex[2].z		= vertices[3]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->ww;
			gVertex[2].u		= minU + cloudOffsetX;;
			gVertex[2].v		= maxV + cloudOffsetY;;
			gVertex[2].argb		= vertices[3]->pVertex->selected ? SELECTION_COLOR : vertices[3]->lightRGB;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			
			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[1].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY; 

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{

						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
				}
			}

			//---------------------------------------
			// Bottom Triangle.
			// gVertex[0] is same as above gVertex[1]
			// gVertex[1] is new and calced from vertex[2].
			// gVertex[2] is same as above.
			gVertex[0].x		= gVertex[1].x;	
			gVertex[0].y		= gVertex[1].y;	
			gVertex[0].z		= gVertex[1].z;	
			gVertex[0].rhw		= gVertex[1].rhw;	
			gVertex[0].u		= gVertex[1].u;	
			gVertex[0].v		= gVertex[1].v;	
			gVertex[0].argb		= gVertex[1].argb;
			gVertex[0].frgb		= gVertex[1].frgb;

			gVertex[1].x		= vertices[2]->wx;
			gVertex[1].y		= vertices[2]->wy;
			gVertex[1].z		= vertices[2]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[2]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;;
			gVertex[1].v		= maxV + cloudOffsetY;;
			gVertex[1].argb		= vertices[2]->pVertex->selected ? SELECTION_COLOR : vertices[2]->lightRGB;
			gVertex[1].frgb		= vertices[2]->fogRGB;
		
			gVertex[0].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[1].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY; 
																   
			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
	
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
						
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{									 
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
						
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
		 
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
long DrawDebugCells = 0;

void TerrainQuad::drawLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.
	long color = XP_WHITE;

	if (uvMode == BOTTOMRIGHT)
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz-0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,vertices[1]->pz-0.002f,1.0f / vertices[1]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[2]->px;
			pos2.y = vertices[2]->py;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz - 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz-0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,vertices[2]->pz-0.002f,1.0f / vertices[2]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz - 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz - 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
	}
	else
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz-0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,vertices[1]->pz-0.002f,1.0f / vertices[1]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;
			pos1.z = vertices[1]->pz - 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz - 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[1]->px,vertices[1]->py,vertices[1]->pz-0.002f,1.0f / vertices[1]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,vertices[2]->pz-0.002f,1.0f / vertices[2]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz - 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz - 0.002f;

			pos2.x = vertices[1]->px;
			pos2.y = vertices[1]->py;
			pos2.z = vertices[1]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
	}

	//------------------------------------------------------------
	// Draw Movement Map Grid.
	//		Once movement is split up, turn this back on for editor -fs
	// I need ALL cells drawn to check elevation Code
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		// Display the ScenarioMap cell grid, as well, displaying open\blocked
		// states...
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		//cellWidth -= 5.0;
		
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = NULL;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (!curCell || 
						curCell->getDebug() || 
						!curCell->getPassable() || 
						curCell->getPathlock(0) || 
						curCell->getDeepWater() || 
						curCell->getShallowWater() ||
						curCell->getForest())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;
						
						Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
						
						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos4);
						
						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos1);
						
						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos3);
						
						pos1.z -= 0.002f;
						pos2.z -= 0.002f;
						pos3.z -= 0.002f;
						pos4.z -= 0.002f;

						DWORD color = XP_RED;
						
						if (!curCell)
						{
							color = XP_GREEN;
						}
						else if (curCell->getDebug() && DrawDebugCells)
						{
							color = XP_YELLOW;
						}
						else if (curCell->getPathlock(0))
						{
							color = XP_YELLOW;
						}
						else if (curCell->getForest())
						{
							color = SB_ORANGE;
						}
						else if (!curCell->getPassable())
						{
							color = SB_RED;
						}
						else if (curCell->getDeepWater())
						{
							color = SB_ORANGE;
						}
						else if (curCell->getShallowWater())
						{
							color = XP_BLUE;
						}

						{
							LineElement newElement(pos1,pos2,color,NULL);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,NULL);
							newElement.draw();
						}
					}
				}
			}
		}
	}

	if (GlobalMoveMap[0]->badLoad)
		return;

	if (clipped1 != 0)
	{
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		cellWidth -= 5.0;
				
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
		
		long cellR = tileR * MAPCELL_DIM;
		long cellC = tileC * MAPCELL_DIM;
		
		for (long currentDoor = 0;currentDoor < GlobalMoveMap[0]->numDoors;currentDoor++)
		{
			if ((GlobalMoveMap[0]->doors[currentDoor].row >= cellR) && 
				(GlobalMoveMap[0]->doors[currentDoor].row < (cellR + MAPCELL_DIM)) &&
				(GlobalMoveMap[0]->doors[currentDoor].col >= cellC) &&
				(GlobalMoveMap[0]->doors[currentDoor].col < (cellC + MAPCELL_DIM)))
			{
				Stuff::Vector4D pos1;
				Stuff::Vector4D pos2;
				Stuff::Vector4D pos3;
				Stuff::Vector4D pos4;
					
				long xLength = 1;
				long yLength = 1;
				
				if (GlobalMoveMap[0]->doors[currentDoor].direction[0] == 1)
				{
					yLength = GlobalMoveMap[0]->doors[currentDoor].length;
				}
				
				if (GlobalMoveMap[0]->doors[currentDoor].direction[0] == 2)
				{
					xLength = GlobalMoveMap[0]->doors[currentDoor].length;
				}

				Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
				
				thePoint.x += (GlobalMoveMap[0]->doors[currentDoor].col - cellC) * cellWidth;
				thePoint.y -= (GlobalMoveMap[0]->doors[currentDoor].row - cellR) * cellWidth;

				thePoint.z = land->getTerrainElevation(thePoint);
				eye->projectZ(thePoint,pos4);
				
				thePoint.x += (xLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				eye->projectZ(thePoint,pos1);
				
				thePoint.y -= (yLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				eye->projectZ(thePoint,pos2);

				thePoint.x -= (xLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				eye->projectZ(thePoint,pos3);

				pos1.z -= 0.002f; 
				pos2.z -= 0.002f;  
				pos3.z -= 0.002f;  
				pos4.z -= 0.002f;
				{
					LineElement newElement(pos1,pos2,XP_GREEN,NULL);
					newElement.draw();
				}	
				
				{
					LineElement newElement(pos2,pos3,XP_GREEN,NULL);
					newElement.draw();
				}
				
				{
					LineElement newElement(pos3,pos4,XP_GREEN,NULL);
					newElement.draw();
				}
				
				{
					LineElement newElement(pos1,pos4,XP_GREEN,NULL);
					newElement.draw();
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------------------------
void TerrainQuad::drawLOSLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.

	//------------------------------------------------------------
	// Draw LOS Map Grid.
	// Draw a color for LOS cell height data based on height.  Draw NOTHING if cell height is ZERO!!
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = NULL;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (curCell && curCell->getLocalHeight())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;
						
						Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
						
						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos4);
						
						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos1);
						
						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos3);
						
						pos1.z -= 0.002f;
						pos2.z -= 0.002f;
						pos3.z -= 0.002f;
						pos4.z -= 0.002f;

						DWORD color = XP_BLACK;
						
						if (curCell->getLocalHeight() < 2)
						{
							color = XP_BLACK;
						}
						else if (curCell->getLocalHeight() < 4)
						{
							color = XP_GRAY;
						}
						else if (curCell->getLocalHeight() < 6)
						{
							color = XP_RED;
						}
						else if (curCell->getLocalHeight() < 8)
						{
							color = XP_ORANGE;
						}
						else if (curCell->getLocalHeight() < 10)
						{
							color = XP_YELLOW;
						}
						else if (curCell->getLocalHeight() < 12)
						{
							color = XP_GREEN;
						}
						else if (curCell->getLocalHeight() < 14)
						{
							color = XP_BLUE;
						}
						else if (curCell->getLocalHeight() <= 16)
						{
							color = XP_WHITE;
						}

						{
							LineElement newElement(pos1,pos2,color,NULL);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,NULL);
							newElement.draw();
						}
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
void TerrainQuad::drawDebugCellLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.

	if (uvMode == BOTTOMRIGHT)
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[2]->px;
			pos2.y = vertices[2]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,HUD_DEPTH,1.0f / vertices[2]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
	}
	else
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,HUD_DEPTH,1.0f / vertices[2]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[1]->px;
			pos2.y = vertices[1]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
	}

	//------------------------------------------------------------
	// Draw Movement Map Grid.
	//		Once movement is split up, turn this back on for editor -fs
	// I need ALL cells drawn to check elevation Code
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		// Display the ScenarioMap cell grid, as well, displaying open\blocked
		// states...
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		//cellWidth -= 5.0;
		
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = NULL;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (!curCell || curCell->getDebug())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;
						
						Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
						
						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos4);
						
						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos1);
						
						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						eye->projectZ(thePoint,pos3);
						
						pos1.z = pos2.z = pos3.z = pos4.z = HUD_DEPTH;

						DWORD color = XP_RED;
						if (!curCell)
						{
							color = XP_GREEN;
						}
						else {
							static DWORD debugColors[4] = {0, XP_RED, XP_WHITE, XP_BLUE};
							DWORD cellDebugValue = curCell->getDebug();
							if (cellDebugValue)
								color = debugColors[cellDebugValue];
						}
/*						else if (curCell->getPathlock())
						{
								color = XP_YELLOW;
						}
						else if (!curCell->getLineOfSight())
						{
					 		color = XP_BLUE;
						}
						else if (!curCell->getPassable())
						{
							color = SB_RED;
						}
*/
						{
							LineElement newElement(pos1,pos2,color,NULL);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,NULL);
							newElement.draw();
						}
					}
				}
			}
		}
	}


}

//---------------------------------------------------------------------------
void TerrainQuad::drawMine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw Mines.
	// All mines are visible all the time!
	if ((clipped1 != 0) || (clipped2 != 0))
	{
		long cellPos = 0;
		float cellWidth = Terrain::worldUnitsPerCell;
		for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
		{
			for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
			{
				//--------------------------------------------------------------------
				bool drawMine = false;
				bool drawBlownMine = false;
				
				if (mineResult.getMine(cellPos) == 1)
					drawMine = true;
								
				if (mineResult.getMine(cellPos) == 2)
					drawBlownMine = true;
								
				if (drawMine || drawBlownMine)
				{
					Stuff::Vector4D pos1;
					Stuff::Vector4D pos2;
					Stuff::Vector4D pos3;
					Stuff::Vector4D pos4;
					
					//------------------------------------------------------------------------------------
					// Dig the actual Vertex information out of the projected vertices already done.
					// In this way, the draw requires only interpolation and not Giant Matrix multiplies.
					Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
					
					thePoint.x += (cellC) * cellWidth;
					thePoint.y -= (cellR) * cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					eye->projectZ(thePoint,pos4);
					
					thePoint.x += cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					eye->projectZ(thePoint,pos1);
					
					thePoint.y -= cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					eye->projectZ(thePoint,pos2);
		
					thePoint.x -= cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					eye->projectZ(thePoint,pos3);
					
					//------------------------------------
					// Replace with New RIA code
					gos_VERTEX gVertex[3];
					gos_VERTEX sVertex[3];
		
					gVertex[0].x        = sVertex[0].x		= pos1.x;
					gVertex[0].y        = sVertex[0].y		= pos1.y;
					gVertex[0].z        = sVertex[0].z		= pos1.z;
					gVertex[0].rhw      = sVertex[0].rhw	= pos1.w;
					gVertex[0].u        = sVertex[0].u		= 0.0f;
					gVertex[0].v        = sVertex[0].v		= 0.0f;
					gVertex[0].argb     = sVertex[0].argb	= vertices[0]->lightRGB;
					gVertex[0].frgb     = sVertex[0].frgb	= vertices[0]->fogRGB;
		
					gVertex[1].x		= pos2.x;
					gVertex[1].y		= pos2.y;
					gVertex[1].z		= pos2.z;
					gVertex[1].rhw		= pos2.w;
					gVertex[1].u		= 0.999999999f;
					gVertex[1].v		= 0.0f;
					gVertex[1].argb		= vertices[1]->lightRGB;
					gVertex[1].frgb		= vertices[1]->fogRGB;
		
					gVertex[2].x        = sVertex[1].x		= pos3.x;
					gVertex[2].y        = sVertex[1].y		= pos3.y;
					gVertex[2].z        = sVertex[1].z		= pos3.z;
					gVertex[2].rhw      = sVertex[1].rhw	= pos3.w;
					gVertex[2].u        = sVertex[1].u		= 0.999999999f;
					gVertex[2].v        = sVertex[1].v		= 0.999999999f;
					gVertex[2].argb     = sVertex[1].argb	= vertices[2]->lightRGB;
					gVertex[2].frgb     = sVertex[1].frgb	= vertices[2]->fogRGB;
		
					sVertex[2].x		= pos4.x;
					sVertex[2].y		= pos4.y;
					sVertex[2].z		= pos4.z;
					sVertex[2].rhw		= pos4.w;
					sVertex[2].u		= 0.0f;
					sVertex[2].v		= 0.999999999f;
					sVertex[2].argb		= vertices[3]->lightRGB;
					sVertex[2].frgb		= vertices[3]->fogRGB;
		
					if ((gVertex[0].z >= 0.0f) &&
						(gVertex[0].z < 1.0f) &&
						(gVertex[1].z >= 0.0f) &&  
						(gVertex[1].z < 1.0f) && 
						(gVertex[2].z >= 0.0f) &&  
						(gVertex[2].z < 1.0f) &&
						(sVertex[0].z >= 0.0f) &&
						(sVertex[0].z < 1.0f) &&
						(sVertex[1].z >= 0.0f) &&  
						(sVertex[1].z < 1.0f) && 
						(sVertex[2].z >= 0.0f) &&  
						(sVertex[2].z < 1.0f))
					{
						if (drawBlownMine)
						{
							mcTextureManager->addVertices(blownTextureHandle,gVertex,MC2_DRAWALPHA);
							mcTextureManager->addVertices(blownTextureHandle,sVertex,MC2_DRAWALPHA);
						}
						else
						{
							mcTextureManager->addVertices(mineTextureHandle,gVertex,MC2_DRAWALPHA);
							mcTextureManager->addVertices(mineTextureHandle,sVertex,MC2_DRAWALPHA);
						}
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
