//===========================================================================//
// devtools.h -- PORT-ADDED. In-game developer panel, opened with INSERT.
//
// Owner ruling 2026-08-21: the retail developer functions are wanted, but not
// as a wall of ctrl+alt+shift chords. They live behind one key and a small
// panel instead, and NEVER-ENDING STORY IS GONE ENTIRELY -- it suppressed every
// mission-ending condition with no way to switch it back off, which is a
// campaign-breaking trap rather than a tool.
//
// This file owns the panel and its input. It does NOT own game state: anything
// that is a plain global it flips directly, and the one thing that is a Mission
// member -- the scenario result -- is handed back as a consume-once request so
// mission.cpp applies it in the scope that owns it.
//
// Gated by MC2_DEV_TOOLS. Off by default, one binary, nothing reachable in a
// normal session.
//===========================================================================//

#ifndef DEVTOOLS_H
#define DEVTOOLS_H

// What the panel wants mission.cpp to do, for the cases the panel cannot do
// itself. Consume-once: reading it clears it.
enum MC2DevRequest
{
	MC2_DEV_REQUEST_NONE = 0,
	MC2_DEV_REQUEST_WIN,
	MC2_DEV_REQUEST_LOSE
};

// Simulation time multiplier, 1.0 unless the panel changed it. >1 is faster.
float         MC2_DevTimeScale     (void);
bool          MC2_DevToolsEnabled   (void);
bool          MC2_DevToolsIsOpen    (void);
void          MC2_DevToolsUpdate    (void);
void          MC2_DevToolsRender    (void);
MC2DevRequest MC2_DevToolsTakeRequest(void);

#endif
