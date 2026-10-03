#include <cstdio>

#include <gameos.hpp>

#include "gos_backend.h"
#include "mc2_port.h"

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", message);
}
}

int main()
{
	Environment.screenWidth = 1920;
	Environment.screenHeight = 1080;
	MC2_SetVirtualCanvas(800, 600);
	gosInput_SetMouse(0.5f, 0.5f, 18, 18, 0, 0);
	float mouseX = 0.0f, mouseY = 0.0f;
	int deltaX = 0, deltaY = 0;
	gos_GetMouseInfo(&mouseX, &mouseY, &deltaX, &deltaY, 0, 0);
	check(mouseX * Environment.screenWidth == 400.0f &&
		mouseY * Environment.screenHeight == 300.0f,
		"virtual canvas maps physical centre to authored centre");
	check(deltaX == 10 && deltaY == 10,
		"virtual canvas inversely scales mouse deltas");
	gos_SetMousePosition(400.0f / Environment.screenWidth,
		300.0f / Environment.screenHeight);
	MC2_SetVirtualCanvas(0, 0);
	gos_GetMouseInfo(&mouseX, &mouseY, 0, 0, 0, 0);
	check(mouseX == 0.5f && mouseY == 0.5f,
		"authored mouse position maps back to physical centre");

    const int key = static_cast<int>(KEY_RETURN);
    check(gos_GetKeyStatus(KEY_RETURN) == KEY_FREE, "key begins free");
    gosInput_SetKey(key, true);
    check(gos_GetKeyStatus(KEY_RETURN) == KEY_PRESSED, "down edge is pressed");
    gosInput_SetKey(key, true);
    check(gos_GetKeyStatus(KEY_RETURN) == KEY_PRESSED, "repeat down preserves pressed edge");
    gosInput_BeginFrame();
    check(gos_GetKeyStatus(KEY_RETURN) == KEY_HELD, "pressed advances to held");
    gosInput_SetKey(key, false);
    check(gos_GetKeyStatus(KEY_RETURN) == KEY_RELEASED, "up edge is released");
    gosInput_BeginFrame();
    check(gos_GetKeyStatus(KEY_RETURN) == KEY_FREE, "released advances to free");
    std::printf("gos input state test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
