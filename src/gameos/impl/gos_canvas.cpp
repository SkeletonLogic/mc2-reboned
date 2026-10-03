//===========================================================================//
// Fixed-layout UI canvas mapping for the native port.
//===========================================================================//

#include <gameos.hpp>

#include "gos_backend.h"
#include "mc2_port.h"

namespace {
DWORD g_canvasWidth = 0;
DWORD g_canvasHeight = 0;

// PORT: see gos_backend.h for why the display size is a registered provider
// rather than a direct call into the platform peer. It is kept in this file
// because this file is already part of mc2_gameos, is tiny, and -- unlike
// gos_platform.cpp -- does not define main().
MC2_DisplaySizeFn g_displaySize = 0;
}

void MC2_SetDisplaySizeProvider(MC2_DisplaySizeFn provider)
{
    g_displaySize = provider;
}

bool MC2_QueryDisplaySize(int* width, int* height)
{
    return g_displaySize ? g_displaySize(width, height) : false;
}

void MC2_SetVirtualCanvas(unsigned int width, unsigned int height)
{
    g_canvasWidth = width;
    g_canvasHeight = height;
}

void MC2_GetVirtualCanvas(unsigned int* width, unsigned int* height)
{
    if (width)  *width  = (unsigned int)g_canvasWidth;
    if (height) *height = (unsigned int)g_canvasHeight;
}

bool MC2_IsVirtualCanvasActive()
{
    return g_canvasWidth && g_canvasHeight;
}

bool gosCanvas_Matches(unsigned int width, unsigned int height)
{
    return g_canvasWidth && g_canvasHeight &&
        width == g_canvasWidth && height == g_canvasHeight;
}

bool gosCanvas_GetTransform(float* scale, float* offsetX, float* offsetY)
{
    float canvasScale = 1.0f;
    float canvasOffsetX = 0.0f;
    float canvasOffsetY = 0.0f;
    const bool active = g_canvasWidth && g_canvasHeight &&
        Environment.screenWidth > 0 && Environment.screenHeight > 0;
    if (active)
    {
        const float scaleX = (float)Environment.screenWidth / (float)g_canvasWidth;
        const float scaleY = (float)Environment.screenHeight / (float)g_canvasHeight;
        canvasScale = scaleX < scaleY ? scaleX : scaleY;
        canvasOffsetX = ((float)Environment.screenWidth - g_canvasWidth * canvasScale) * 0.5f;
        canvasOffsetY = ((float)Environment.screenHeight - g_canvasHeight * canvasScale) * 0.5f;
    }
    if (scale) *scale = canvasScale;
    if (offsetX) *offsetX = canvasOffsetX;
    if (offsetY) *offsetY = canvasOffsetY;
    return active;
}

void gosCanvas_PhysicalToGame(float* x, float* y, int* dx, int* dy)
{
    float scale = 1.0f, offsetX = 0.0f, offsetY = 0.0f;
    if (!gosCanvas_GetTransform(&scale, &offsetX, &offsetY)) return;
    if (x) *x = ((*x * Environment.screenWidth) - offsetX) /
        scale / Environment.screenWidth;
    if (y) *y = ((*y * Environment.screenHeight) - offsetY) /
        scale / Environment.screenHeight;
    if (dx) *dx = (int)(*dx / scale);
    if (dy) *dy = (int)(*dy / scale);
}

void gosCanvas_GameToPhysical(float* x, float* y)
{
    float scale = 1.0f, offsetX = 0.0f, offsetY = 0.0f;
    if (!gosCanvas_GetTransform(&scale, &offsetX, &offsetY)) return;
    if (x) *x = (*x * Environment.screenWidth * scale + offsetX) /
        Environment.screenWidth;
    if (y) *y = (*y * Environment.screenHeight * scale + offsetY) /
        Environment.screenHeight;
}
