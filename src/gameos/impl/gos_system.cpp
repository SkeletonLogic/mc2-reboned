//===========================================================================//
// GameOS system services -- machine info, screen mode, viewport, registry,
// keyboard/date/path helpers, and the joystick stubs.
//
// Clean-room reimplementation from Source/GameOS/include/*.
//
// Everything here is either genuinely portable or a deliberate stub that
// reports rather than lying. Nothing in this file touches the renderer: screen
// mode and viewport only record state into Environment so the game's own
// queries stay self-consistent until a real renderer exists.
//===========================================================================//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <gameos.hpp>
#include <toolos.hpp>

#include "gos_backend.h"
#include "mc2_port.h"

namespace {

void reportOnce(const char* fn)
{
    static std::map<std::string, bool>* seen = new std::map<std::string, bool>();
    if ((*seen)[fn]) return;
    (*seen)[fn] = true;
    std::fprintf(stderr, "[GameOS] %s is stubbed in this build\n", fn);
}

//---------------------------------------------------------------------------//
// Registry replacement.
//
// GameOS persisted settings under HKCU. This is a flat key/value store in a
// single file next to the executable, which is portable and maps directly onto
// IDBFS/localStorage for the web build later.
//---------------------------------------------------------------------------//
typedef std::map<std::string, std::vector<unsigned char> > SettingsMap;

SettingsMap& settings()
{
    static SettingsMap* s = new SettingsMap();
    return *s;
}

const char* settingsPath() { return "mc2_settings.bin"; }

bool g_settingsLoaded = false;

void loadSettings()
{
    if (g_settingsLoaded) return;
    g_settingsLoaded = true;

    std::FILE* f = std::fopen(settingsPath(), "rb");
    if (!f) return;

    for (;;)
    {
        unsigned int nameLen = 0, dataLen = 0;
        if (std::fread(&nameLen, sizeof(nameLen), 1, f) != 1) break;
        if (nameLen == 0 || nameLen > 1024) break;

        std::string name(nameLen, '\0');
        if (std::fread(&name[0], 1, nameLen, f) != nameLen) break;
        if (std::fread(&dataLen, sizeof(dataLen), 1, f) != 1) break;
        if (dataLen > (1u << 20)) break;

        std::vector<unsigned char> data(dataLen);
        if (dataLen && std::fread(&data[0], 1, dataLen, f) != dataLen) break;

        settings()[name] = data;
    }
    std::fclose(f);
}

void saveSettings()
{
    std::FILE* f = std::fopen(settingsPath(), "wb");
    if (!f) return;

    for (SettingsMap::const_iterator it = settings().begin(); it != settings().end(); ++it)
    {
        unsigned int nameLen = (unsigned int)it->first.size();
        unsigned int dataLen = (unsigned int)it->second.size();
        std::fwrite(&nameLen, sizeof(nameLen), 1, f);
        std::fwrite(it->first.data(), 1, nameLen, f);
        std::fwrite(&dataLen, sizeof(dataLen), 1, f);
        if (dataLen) std::fwrite(&it->second[0], 1, dataLen, f);
    }
    std::fclose(f);
}

} // namespace

//---------------------------------------------------------------------------//
// Machine information
//
// GameOS.HPP documents gos_GetMachineInformation as a general capability query
// keyed by the MachineInfo enum. Returning 0 for anything not explicitly
// answered is the conservative choice: it reads as "capability absent", which
// pushes the game onto its most portable code path.
//
// The specific queries the game actually issues will be filled in as it
// exercises them -- gos_Info_NumberDevices and the screen dimensions are
// answered here because startup reads them before anything else.
//---------------------------------------------------------------------------//
uintptr_t __stdcall gos_GetMachineInformation(MachineInfo mi, int /*P1*/, int P2, int P3, int /*P4*/)
{
    switch ((int)mi)
    {
        case gos_Info_NumberDevices:        return 1;
        case gos_Info_GetDeviceLocalMemory: return 256u * 1024u * 1024u;
        case gos_Info_GetDeviceAGPMemory:   return 0;

        // PORT: the options screen was empty because this returned 0 for
        // everything. OptionsArea.cpp builds the resolution list by asking
        // gos_Info_ValidMode for each of its ten modes and DROPPING every one
        // that does not answer exactly 1, and builds the video-card list from
        // gos_Info_GetDeviceName, which it casts straight to char* -- so a 0
        // was both an empty list and a null pointer handed to AddItem.
        //
        // The renderer presents whatever size Environment carries, so every mode
        // the DISPLAY can show is genuinely valid -- but "every mode" is not the
        // same as "all of them". An unconditional 1 was right while the table
        // held ten 4:3/5:4 modes any adapter can do. Once the port added a modern
        // catalogue reaching 5120x2880 it meant a 1080p player was offered 5K,
        // the list was not filtered by anything at all, and applyPrefs would hand
        // an impossible size straight to gos_SetScreenMode.
        //
        // So: bounded by the primary display, with a floor at the smallest mode
        // the game's own art is authored for. A host that cannot report a display
        // size still answers 1 -- refusing on "don't know" would empty the
        // options list and break the headless regression runs, which have no
        // display at all.
        case gos_Info_ValidMode:
        {
            if (P2 < 640 || P3 < 480) return 0;

            int displayWidth = 0, displayHeight = 0;
            if (!MC2_QueryDisplaySize(&displayWidth, &displayHeight)) return 1;

            return (P2 <= displayWidth && P3 <= displayHeight) ? 1 : 0;
        }

        // PORT: this query is pointer-valued. The public return type is now
        // uintptr_t, so the options screen can enumerate the port adapter on
        // both 32- and 64-bit targets without truncating its address.
        case gos_Info_GetDeviceName:
            return reinterpret_cast<uintptr_t>("MechCommander 2 Display Adapter");

        default:                            return 0;
    }
}

//---------------------------------------------------------------------------//
// Screen mode / viewport
//
// No renderer yet, so these record intent into Environment rather than doing
// anything. Recording matters: the game reads Environment.screenWidth/Height
// constantly for UI layout, so if gos_SetScreenMode silently did nothing the
// entire interface would lay out against stale values.
//---------------------------------------------------------------------------//
// PORT: MC2 can only ask for the five modes hard-coded in prefs.cpp
// (640x480, 800x600, 1024x768, 1280x1024, 1600x1200) -- all 4:3 or 5:4, and
// there is no path by which the game requests anything else. MC2_SCREEN=WxH
// overrides whatever mode the game asks for, which is the only way to get a
// widescreen backbuffer without editing the game's mode table.
//
// Everything downstream already follows Environment: the WebGPU target is sized
// from screenWidth/Height each frame, and Camera::setOrthogonal /
// setPerspective derive their clip planes from screenHeight/screenWidth. So the
// 3D view is aspect-correct at any size. The 2D UI is NOT -- it is authored at
// absolute pixel coordinates per width. Silent by default.
static bool ScreenOverride(DWORD& width, DWORD& height)
{
    static int   parsed = -1;
    static DWORD overrideWidth = 0, overrideHeight = 0;
    if (parsed < 0)
    {
        parsed = 0;
        const char* want = std::getenv("MC2_SCREEN");
        if (want && *want)
        {
            unsigned w = 0, h = 0;
            if (std::sscanf(want, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240)
            {
                overrideWidth  = w;
                overrideHeight = h;
                parsed = 1;
                std::fprintf(stderr, "[GameOS] MC2_SCREEN=%ux%u overrides every mode request\n", w, h);
            }
            else
            {
                std::fprintf(stderr, "[GameOS] MC2_SCREEN must look like 1920x1080\n");
            }
        }
    }
    if (parsed != 1) return false;
    width  = overrideWidth;
    height = overrideHeight;
    return true;
}

void __stdcall gos_SetScreenMode(DWORD Width, DWORD Height, DWORD bitDepth, DWORD Device,
                                 bool disableZBuffer, bool AntiAlias, bool RenderToVram,
                                 bool GotoFullScreen, int DirtyRectangle, bool GotoWindowMode,
                                 bool EnableStencil, DWORD Renderer)
{
    const DWORD requestedWidth = Width;
    const DWORD requestedHeight = Height;
    const bool overridden = ScreenOverride(Width, Height);

    // PORT: an active fixed-layout canvas changes the UI coordinate system,
    // not the physical backbuffer. Preserve the selected/overridden mode when
    // legacy logistics code asks GameOS to switch back to authored 800x600.
    if (!overridden && gosCanvas_Matches(requestedWidth, requestedHeight) &&
        Environment.screenWidth > 0 && Environment.screenHeight > 0)
    {
        Width = (DWORD)Environment.screenWidth;
        Height = (DWORD)Environment.screenHeight;
    }

    Environment.screenWidth      = (int)Width;
    Environment.screenHeight     = (int)Height;
    Environment.bitDepth         = (int)bitDepth;
    Environment.FullScreenDevice = (int)Device;
    Environment.disableZBuffer   = disableZBuffer;
    Environment.AntiAlias        = AntiAlias;
    Environment.RenderToVram     = RenderToVram;
    Environment.DirtyRectangle   = DirtyRectangle;
    Environment.Stencil          = EnableStencil;
    Environment.Renderer         = (int)Renderer;

    if (GotoFullScreen)  Environment.fullScreen = true;
    if (GotoWindowMode)  Environment.fullScreen = false;
}

namespace {
    float g_vpMulX = 1.0f, g_vpMulY = 1.0f, g_vpAddX = 0.0f, g_vpAddY = 0.0f;
}

void __stdcall gos_SetViewportBackground(bool Fill, DWORD Color);

void __stdcall gos_SetupViewport(bool /*FillZ*/, float /*ZBuffer*/, bool FillBG, DWORD BGColor,
                                 float top, float left, float bottom, float right,
                                 bool /*ClearStencil*/, DWORD /*StencilValue*/)
{
    // PORT (M8-F1 follow-up): these two were discarded. See
    // RendererBackend::SetViewportBackground -- throwing them away is why the
    // frame cleared to black above the sky instead of to the fog colour the
    // game has always asked for.
    gos_SetViewportBackground(FillBG, BGColor);

    // GameOS accepts normalized viewport bounds but returns the pixel-space
    // multiplier/offset used by pre-transformed gos_VERTEX coordinates.
    const float width = static_cast<float>(Environment.screenWidth);
    const float height = static_cast<float>(Environment.screenHeight);
    g_vpMulX = (right - left) * width;
    g_vpMulY = (bottom - top) * height;
    g_vpAddX = left * width;
    g_vpAddY = top * height;
}

void __stdcall gos_GetViewport(float* pViewportMulX, float* pViewportMulY,
                               float* pViewportAddX, float* pViewportAddY)
{
    if (pViewportMulX) *pViewportMulX = g_vpMulX;
    if (pViewportMulY) *pViewportMulY = g_vpMulY;
    if (pViewportAddX) *pViewportAddX = g_vpAddX;
    if (pViewportAddY) *pViewportAddY = g_vpAddY;
}

DWORD __stdcall gos_EnableSetting(gosSetting /*Setting*/, DWORD /*Value*/)
{
    return 0;
}

//---------------------------------------------------------------------------//
// Application lifetime
//---------------------------------------------------------------------------//
void __stdcall gos_TerminateApplication()
{
    // Ends the main loop cleanly rather than calling exit(), so
    // TerminateGameEngine still runs and the shutdown path stays exercised.
    g_gosQuitRequested = true;
}

//---------------------------------------------------------------------------//
// Keyboard helpers (the real input path lives in the input subsystem)
//---------------------------------------------------------------------------//
char* __stdcall gos_DescribeKey(DWORD Key)
{
    static char buf[32];
    if (Key >= 'A' && Key <= 'Z') { buf[0] = (char)Key; buf[1] = '\0'; return buf; }
    std::snprintf(buf, sizeof(buf), "Key%lu", (unsigned long)Key);
    return buf;
}

void __stdcall gos_KeyboardFlush()
{
}

//---------------------------------------------------------------------------//
// Date / time formatting
//---------------------------------------------------------------------------//
char* __stdcall gos_GetFormattedDate(bool Verbose, WORD Year, WORD Month, WORD Day)
{
    static char buf[64];
    std::time_t now = std::time(nullptr);
    std::tm t = {};
#if defined(_WIN32)
    localtime_s(&t, &now);
#else
    t = *std::localtime(&now);
#endif
    int y = (Year  == (WORD)-1) ? t.tm_year + 1900 : Year;
    int m = (Month == (WORD)-1) ? t.tm_mon + 1     : Month;
    int d = (Day   == (WORD)-1) ? t.tm_mday        : Day;

    if (Verbose) std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    else         std::snprintf(buf, sizeof(buf), "%02d/%02d/%04d", m, d, y);
    return buf;
}

char* __stdcall gos_GetFormattedTime(WORD Hour, WORD Minute, WORD Second)
{
    static char buf[32];
    std::time_t now = std::time(nullptr);
    std::tm t = {};
#if defined(_WIN32)
    localtime_s(&t, &now);
#else
    t = *std::localtime(&now);
#endif
    int h = (Hour   == (WORD)-1) ? t.tm_hour : Hour;
    int mi= (Minute == (WORD)-1) ? t.tm_min  : Minute;
    int s = (Second == (WORD)-1) ? t.tm_sec  : Second;
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, mi, s);
    return buf;
}

//---------------------------------------------------------------------------//
// Paths / disk
//---------------------------------------------------------------------------//
void __stdcall gos_GetCurrentPath(char* Buffer, int buf_len)
{
    if (!Buffer || buf_len <= 0) return;
    // The VFS (next subsystem) roots everything at the data directory, so the
    // process working directory is the correct answer here.
#if defined(_WIN32)
    if (!_getcwd(Buffer, buf_len)) { Buffer[0] = '.'; Buffer[1] = '\0'; }
#else
    if (!getcwd(Buffer, (size_t)buf_len)) { Buffer[0] = '.'; Buffer[1] = '\0'; }
#endif
}

__int64 __stdcall gos_GetDriveFreeSpace(char* /*Path*/)
{
    // Only used to warn about low disk space before saving. Reporting a large
    // value skips a warning the port cannot meaningfully answer yet.
    return (__int64)8 * 1024 * 1024 * 1024;
}

//---------------------------------------------------------------------------//
// Registry replacement
//---------------------------------------------------------------------------//
void __stdcall gos_SaveDataToRegistry(char* keyName, void* pData, DWORD szData)
{
    if (!keyName || !pData) return;
    loadSettings();
    const unsigned char* p = (const unsigned char*)pData;
    settings()[keyName].assign(p, p + szData);
    saveSettings();
}

void __stdcall gos_LoadDataFromRegistry(char* keyName, void* pData, DWORD* szData, bool /*HKLM*/)
{
    if (!keyName || !szData) return;
    loadSettings();

    SettingsMap::const_iterator it = settings().find(keyName);
    if (it == settings().end()) { *szData = 0; return; }

    DWORD avail = (DWORD)it->second.size();
    DWORD copy  = (*szData < avail) ? *szData : avail;
    if (pData && copy) std::memcpy(pData, &it->second[0], copy);
    *szData = copy;
}

//---------------------------------------------------------------------------//
// Joystick -- stubbed.
//
// Phase 0 never listed these; the linker found them. MC2 supports a joystick
// but the port targets keyboard and mouse, so these report "no devices" which
// is a state the game already handles.
//---------------------------------------------------------------------------//
DWORD __stdcall gosJoystick_CountJoysticks(bool /*ReDetect*/)
{
    return 0;
}

void __stdcall gosJoystick_GetInfo(DWORD /*index*/, gosJoystick_Info* gji)
{
    if (gji) std::memset(gji, 0, sizeof(*gji));
}

void __stdcall gosJoystick_SetPolling(DWORD /*index*/, bool /*yesNo*/, float /*howOften*/)
{
}

float __stdcall gosJoystick_GetAxis(DWORD /*index*/, GOSJoystickAxis /*axis*/)
{
    return 0.0f;
}
