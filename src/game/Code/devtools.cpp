//===========================================================================//
// devtools.cpp -- PORT-ADDED. See devtools.h for why this exists.
//
// Drawn with the port's own primitives -- drawRect from Utilities and the gos
// text calls -- so NO RETAIL .fit OR TEXTURE IS TOUCHED. That constraint comes
// from M7-B14 and it still holds: the authored art must not be modified or
// redistributed to add a port feature.
//
// EVERY ACTION ANNOUNCES ITSELF ON stderr. That is not decoration. A defect
// reported from a session that ran at 10x speed, or had collisions off, is a
// DIFFERENT REPORT from one found in normal play, and the log has to say which
// it was or the evidence is ambiguous.
//===========================================================================//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devtools.h"

#ifndef MECHCLASS_H
#include "mech.h"
#endif

#ifndef USERINPUT_H
#include "userinput.h"
#endif

#ifndef UTILITIES_H
#include "Utilities.h"
#endif

#ifndef LOGISTICSDATA_H
#include "LogisticsData.h"
#endif

//---------------------------------------------------------------------------
// State owned here.

// OWNER REPORT 2026-08-21: "game speed amplifier slows the game instead of
// increasing the speed". Correct, and the cause is in the arithmetic rather
// than anywhere subtle -- MEASURED BY READING, not guessed:
//
//   frameLength = 1.0 / frameRate      seconds of SIMULATION per frame
//   frameLength /= forcedFrameRate     retail, mission.cpp
//
// Dividing the time step means LESS game time advances per rendered frame, so
// retail's "forcedFrameRate" is a SLOW-MOTION control. At 2 it runs at half
// speed. I labelled it 2x/5x/10x and had the direction exactly backwards.
//
// So the panel now drives a real MULTIPLIER and mission.cpp applies it as one.
// Slow motion is kept because it is genuinely useful for watching effects --
// it is just labelled honestly now.
// s_timeScale is the value. Retail forcedFrameRate is left alone and is
// inert -- nothing has set it since the key chords went.
extern bool  useCollisions;        // mission.cpp
extern bool  useSensors;           // mission.cpp
extern bool  DisplayCameraAngle;   // mission.cpp
extern bool  saveInMissionSave;    // mission.cpp
extern bool  loadInMissionSave;    // mission.cpp

static float         s_timeScale = 1.0f;
static bool          s_open    = false;
static bool          s_wasDown = false;
static bool          s_mouseWasDown = false;
static MC2DevRequest s_request = MC2_DEV_REQUEST_NONE;

// Win and lose end the mission outright and cannot be taken back, so they ask
// first. NOTHING ELSE ON THE PANEL DOES. Speed, collisions, sensors and the
// camera readout all toggle straight back; resource points and the in-mission
// save are not destructive. Confirming those too would only train the habit of
// clicking through the box without reading it, which is worse than no box.
static MC2DevRequest s_confirm = MC2_DEV_REQUEST_NONE;

// Panel geometry. Top-left, out of the way of the tactical dock at the bottom.
static const long PANEL_LEFT   = 24;
static const long PANEL_TOP    = 60;
static const long PANEL_WIDTH  = 260;
static const long ROW_HEIGHT   = 18;
static const long ROW_INSET    = 8;

// Confirm box, drawn over the panel. One place for the geometry so the drawing
// and the hit test cannot disagree about where the buttons are.
static const long CONFIRM_TOP     = 96;
static const long CONFIRM_HEIGHT  = 78;
static const long CONFIRM_BTN_TOP = CONFIRM_TOP + 46;
static const long CONFIRM_BTN_H   = 22;
static const long CONFIRM_YES_L   = PANEL_LEFT + 24;
static const long CONFIRM_YES_R   = PANEL_LEFT + 114;
static const long CONFIRM_NO_L    = PANEL_LEFT + 146;
static const long CONFIRM_NO_R    = PANEL_LEFT + 236;

enum DevRow
{
	ROW_SPEED = 0,
	ROW_WIN,
	ROW_LOSE,
	ROW_RESOURCE,
	ROW_SAVE,
	ROW_LOAD,
	ROW_COLLISIONS,
	ROW_SENSORS,
	ROW_CAMERA,
	ROW_CLOSE,
	ROW_COUNT
};


static void devLog (const char* what)
{
	fprintf(stderr, "[devtools] %s\n", what);
	fflush(stderr);
}

// MC2_DEV_TIME_SCALE=<n> sets the starting scale. The panel cannot be driven
// headless, so without this the speed fix could only be checked by asking the
// owner to look -- and "it feels faster" is not a measurement. It also makes a
// given speed repeatable across runs, which clicking never is.
float MC2_DevTimeScale (void)
{
	static bool read = false;
	if (!read)
	{
		read = true;
		const char* want = getenv("MC2_DEV_TIME_SCALE");
		if (want && *want)
		{
			const float value = (float)atof(want);
			if (value > 0.0f)
			{
				s_timeScale = value;
				char note[96];
				sprintf(note, "time scale preset to %.2fx from MC2_DEV_TIME_SCALE", value);
				devLog(note);
			}
		}
	}
	return s_timeScale;
}

//---------------------------------------------------------------------------
bool MC2_DevToolsEnabled (void)
{
	static int enabled = -1;
	if (enabled < 0)
	{
		const char* want = getenv("MC2_DEV_TOOLS");
		enabled = (want && *want && strcmp(want, "0") != 0) ? 1 : 0;
		if (enabled)
			devLog("armed -- press INSERT in a mission to open the panel");
	}
	return enabled != 0;
}

//---------------------------------------------------------------------------
bool MC2_DevToolsIsOpen (void)
{
	return MC2_DevToolsEnabled() && s_open;
}

//---------------------------------------------------------------------------
MC2DevRequest MC2_DevToolsTakeRequest (void)
{
	const MC2DevRequest taken = s_request;
	s_request = MC2_DEV_REQUEST_NONE;
	return taken;
}

//---------------------------------------------------------------------------
static const char* speedText (void)
{
	if (s_timeScale == 0.25f) return "0.25x  (slow)";
	if (s_timeScale == 0.5f)  return "0.5x   (slow)";
	if (s_timeScale == 2.0f)  return "2x     (fast)";
	if (s_timeScale == 5.0f)  return "5x     (fast)";
	if (s_timeScale == 10.0f) return "10x    (fast)";
	return "normal";
}

//---------------------------------------------------------------------------
// One row of text. Kept in one place so the panel and the hit test cannot
// disagree about what is on which line.
static void rowLabel (long row, char* out, long outSize)
{
	switch (row)
	{
	case ROW_SPEED:
		sprintf(out, "Game speed:     %s", speedText());
		break;
	case ROW_WIN:        strcpy(out, "Win mission now");            break;
	case ROW_LOSE:       strcpy(out, "Lose mission now");           break;
	case ROW_RESOURCE:   strcpy(out, "Resource points = 100,000");  break;
	case ROW_SAVE:       strcpy(out, "Save in-mission");            break;
	case ROW_LOAD:       strcpy(out, "Load in-mission save");       break;
	case ROW_COLLISIONS:
		sprintf(out, "Collisions:     %s", useCollisions ? "on" : "OFF");
		break;
	case ROW_SENSORS:
		sprintf(out, "Sensors:        %s", useSensors ? "on" : "OFF");
		break;
	case ROW_CAMERA:
		sprintf(out, "Camera angle:   %s", DisplayCameraAngle ? "shown" : "hidden");
		break;
	case ROW_CLOSE:      strcpy(out, "Close  (INSERT)");            break;
	default:             out[0] = 0;                                break;
	}
	(void)outSize;
}

//---------------------------------------------------------------------------
static void activate (long row)
{
	char note[160];
	switch (row)
	{
	case ROW_SPEED:
		// normal -> faster -> slower -> normal, one click a step.
		if (s_timeScale == 1.0f)       s_timeScale = 2.0f;
		else if (s_timeScale == 2.0f)  s_timeScale = 5.0f;
		else if (s_timeScale == 5.0f)  s_timeScale = 10.0f;
		else if (s_timeScale == 10.0f) s_timeScale = 0.5f;
		else if (s_timeScale == 0.5f)  s_timeScale = 0.25f;
		else                           s_timeScale = 1.0f;
		sprintf(note, "game speed now %s", speedText());
		devLog(note);
		break;

	case ROW_WIN:
		s_confirm = MC2_DEV_REQUEST_WIN;
		devLog("WIN clicked -- waiting for confirmation");
		break;

	case ROW_LOSE:
		s_confirm = MC2_DEV_REQUEST_LOSE;
		devLog("LOSE clicked -- waiting for confirmation");
		break;

	case ROW_RESOURCE:
		if (LogisticsData::instance)
		{
			LogisticsData::instance->setResourcePoints(100000);
			devLog("resource points set to 100000");
		}
		break;

	case ROW_SAVE:
		saveInMissionSave = true;
		devLog("in-mission SAVE requested");
		break;

	case ROW_LOAD:
		loadInMissionSave = true;
		devLog("in-mission LOAD requested");
		break;

	case ROW_COLLISIONS:
		useCollisions = !useCollisions;
		sprintf(note, "collisions %s", useCollisions ? "on" : "OFF");
		devLog(note);
		break;

	case ROW_SENSORS:
		useSensors = !useSensors;
		sprintf(note, "sensors %s", useSensors ? "on" : "OFF");
		devLog(note);
		break;

	case ROW_CAMERA:
		DisplayCameraAngle = !DisplayCameraAngle;
		sprintf(note, "camera-angle readout %s", DisplayCameraAngle ? "shown" : "hidden");
		devLog(note);
		break;

	case ROW_CLOSE:
		s_open = false;
		devLog("panel closed");
		break;

	default:
		break;
	}
}

//---------------------------------------------------------------------------
void MC2_DevToolsUpdate (void)
{
	if (!MC2_DevToolsEnabled() || !userInput)
		return;

	// INSERT toggles, on the EDGE. Without the edge test the panel flickers
	// open and shut for as long as the key is held.
	const bool down = userInput->getKeyDown(KEY_INSERT);
	if (down && !s_wasDown)
	{
		if (s_confirm != MC2_DEV_REQUEST_NONE)
		{
			// INSERT cancels a pending confirmation rather than closing the
			// panel around it. Closing with an unanswered question would leave
			// the next open showing a box the player did not ask for.
			s_confirm = MC2_DEV_REQUEST_NONE;
			devLog("confirmation cancelled");
		}
		else
		{
			s_open = !s_open;
			devLog(s_open ? "panel opened" : "panel closed");
		}
	}
	s_wasDown = down;

	if (!s_open)
	{
		s_confirm = MC2_DEV_REQUEST_NONE;
		s_mouseWasDown = false;
		return;
	}

	// Left click on a row. Edge-triggered for the same reason.
	// getMouseLeftButtonState, not the member: leftMouseButtonState is protected.
	const bool mouseDown = (userInput->getMouseLeftButtonState() == MC2_MOUSE_DOWN);
	if (mouseDown && !s_mouseWasDown)
	{
		const long mx = (long)userInput->getMouseX();
		const long my = (long)userInput->getMouseY();

		if (s_confirm != MC2_DEV_REQUEST_NONE)
		{
			// While the box is up it takes EVERY click. The rows underneath are
			// unreachable, so a stray click cannot fire a second action while a
			// question is still open.
			if (my >= CONFIRM_BTN_TOP && my < CONFIRM_BTN_TOP + CONFIRM_BTN_H)
			{
				if (mx >= CONFIRM_YES_L && mx <= CONFIRM_YES_R)
				{
					s_request = s_confirm;
					devLog(s_confirm == MC2_DEV_REQUEST_WIN
						? "CONFIRMED -- instant win"
						: "CONFIRMED -- instant loss");
					s_confirm = MC2_DEV_REQUEST_NONE;
				}
				else if (mx >= CONFIRM_NO_L && mx <= CONFIRM_NO_R)
				{
					s_confirm = MC2_DEV_REQUEST_NONE;
					devLog("cancelled");
				}
			}
		}
		else if (mx >= PANEL_LEFT && mx <= PANEL_LEFT + PANEL_WIDTH)
		{
			const long row = (my - PANEL_TOP) / ROW_HEIGHT;
			if (row >= 0 && row < ROW_COUNT)
				activate(row);
		}
	}
	s_mouseWasDown = mouseDown;
}

//---------------------------------------------------------------------------
void MC2_DevToolsRender (void)
{
	if (!MC2_DevToolsEnabled() || !s_open)
		return;

	const long panelHeight = ROW_HEIGHT * ROW_COUNT + ROW_INSET;

	GUI_RECT panel;
	panel.left   = (short)PANEL_LEFT;
	panel.top    = (short)(PANEL_TOP - ROW_INSET);
	panel.right  = (short)(PANEL_LEFT + PANEL_WIDTH);
	panel.bottom = (short)(PANEL_TOP + panelHeight);
	drawRect(panel, 0xC0000000);

	const long mx = (long)userInput->getMouseX();
	const long my = (long)userInput->getMouseY();
	const bool confirming = (s_confirm != MC2_DEV_REQUEST_NONE);
	// No row highlight while the box is up: the rows are not clickable then, and
	// highlighting something that will not respond is a lie.
	const bool overPanel = !confirming &&
		(mx >= PANEL_LEFT && mx <= PANEL_LEFT + PANEL_WIDTH);

	gos_TextSetAttributes(gosFontHandle, 0xFFFFFFFF, gosFontScale,
	                      false, true, false, false);
	gos_TextSetRegion(0, 0, Environment.screenWidth, Environment.screenHeight);

	gos_TextSetPosition((int)(PANEL_LEFT + ROW_INSET), (int)(PANEL_TOP - ROW_INSET + 2));
	gos_TextDraw("DEV TOOLS");

	for (long row = 0; row < ROW_COUNT; row++)
	{
		const long rowTop = PANEL_TOP + row * ROW_HEIGHT;

		// Highlight the row under the cursor so it is obvious what a click hits.
		if (overPanel && my >= rowTop && my < rowTop + ROW_HEIGHT)
		{
			GUI_RECT hot;
			hot.left   = (short)(PANEL_LEFT + 2);
			hot.top    = (short)rowTop;
			hot.right  = (short)(PANEL_LEFT + PANEL_WIDTH - 2);
			hot.bottom = (short)(rowTop + ROW_HEIGHT);
			drawRect(hot, 0x60FFFFFF);
		}

		char label[160];
		rowLabel(row, label, sizeof(label));
		gos_TextSetPosition((int)(PANEL_LEFT + ROW_INSET), (int)(rowTop + 2));
		gos_TextDraw(label);
	}

	if (!confirming)
		return;

	// The confirm box, drawn LAST so it sits over the rows it disables.
	GUI_RECT box;
	box.left   = (short)PANEL_LEFT;
	box.top    = (short)CONFIRM_TOP;
	box.right  = (short)(PANEL_LEFT + PANEL_WIDTH);
	box.bottom = (short)(CONFIRM_TOP + CONFIRM_HEIGHT);
	drawRect(box, 0xF0100000);

	gos_TextSetPosition((int)(PANEL_LEFT + ROW_INSET), (int)(CONFIRM_TOP + 6));
	gos_TextDraw(s_confirm == MC2_DEV_REQUEST_WIN
		? "Win this mission immediately?"
		: "Lose this mission immediately?");
	gos_TextSetPosition((int)(PANEL_LEFT + ROW_INSET), (int)(CONFIRM_TOP + 24));
	gos_TextDraw("ARE YOU SURE?  This cannot be undone.");

	const bool overYes = (my >= CONFIRM_BTN_TOP && my < CONFIRM_BTN_TOP + CONFIRM_BTN_H &&
	                      mx >= CONFIRM_YES_L && mx <= CONFIRM_YES_R);
	const bool overNo  = (my >= CONFIRM_BTN_TOP && my < CONFIRM_BTN_TOP + CONFIRM_BTN_H &&
	                      mx >= CONFIRM_NO_L && mx <= CONFIRM_NO_R);

	GUI_RECT yes;
	yes.left   = (short)CONFIRM_YES_L;
	yes.top    = (short)CONFIRM_BTN_TOP;
	yes.right  = (short)CONFIRM_YES_R;
	yes.bottom = (short)(CONFIRM_BTN_TOP + CONFIRM_BTN_H);
	drawRect(yes, overYes ? 0xA0FF4040 : 0x60802020);

	GUI_RECT no;
	no.left   = (short)CONFIRM_NO_L;
	no.top    = (short)CONFIRM_BTN_TOP;
	no.right  = (short)CONFIRM_NO_R;
	no.bottom = (short)(CONFIRM_BTN_TOP + CONFIRM_BTN_H);
	drawRect(no, overNo ? 0xA0FFFFFF : 0x60606060);

	gos_TextSetPosition((int)(CONFIRM_YES_L + 26), (int)(CONFIRM_BTN_TOP + 4));
	gos_TextDraw("YES");
	gos_TextSetPosition((int)(CONFIRM_NO_L + 30), (int)(CONFIRM_BTN_TOP + 4));
	gos_TextDraw("NO");
}
