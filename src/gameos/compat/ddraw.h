#pragma once
// PORT: Compile-time DirectDraw surface contract for the headless renderer.
#include <windows.h>

struct DDPIXELFORMAT
{
	DWORD dwSize = sizeof(DDPIXELFORMAT);
	DWORD dwRGBBitCount = 32;
	DWORD dwRBitMask = 0x00ff0000;
	DWORD dwGBitMask = 0x0000ff00;
	DWORD dwBBitMask = 0x000000ff;
};

struct _DDSURFACEDESC2
{
	DWORD dwSize = sizeof(_DDSURFACEDESC2);
	LONG lPitch = 0;
	LPVOID lpSurface = nullptr;
	DDPIXELFORMAT ddpfPixelFormat;
};
using DDSURFACEDESC2 = _DDSURFACEDESC2;

class IDirectDrawSurface7
{
public:
	virtual HRESULT Lock(RECT*, DDSURFACEDESC2*, DWORD, HANDLE) = 0;
	virtual HRESULT Unlock(RECT*) = 0;
protected:
	~IDirectDrawSurface7() = default;
};

#define DD_OK S_OK
#define DDLOCK_DONOTWAIT 0x00004000L
#define DDLOCK_WAIT 0x00000001L
