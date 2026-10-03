//===========================================================================//
// Win32 platform backend. This is the only platform implementation allowed to
// include windows.h; SDL2 and Emscripten will be peers behind PlatformBackend.
//===========================================================================//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mc2_port.h"   // PORT (M8-B18): MC2_PublishedTurn
#include <windowsx.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include <gameos.hpp>

#include "gos_backend.h"
#include "platform/platform_backend.h"

extern HWND hWindow;
extern POINT clientToScreen;

namespace {

LONG WINAPI CrashTrace(EXCEPTION_POINTERS* exception)
{
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != STATUS_STACK_OVERFLOW &&
        code != STATUS_HEAP_CORRUPTION)
        return EXCEPTION_CONTINUE_SEARCH;

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    const DWORD64 imageBase = reinterpret_cast<DWORD64>(GetModuleHandleA(nullptr));
    const DWORD64 address = reinterpret_cast<DWORD64>(exception->ExceptionRecord->ExceptionAddress);
    std::fprintf(stderr,
                 "[platform-win32] crash code=0x%08lX address=0x%llX imageRva=0x%llX\n",
                 static_cast<unsigned long>(code), address,
                 address >= imageBase ? address - imageBase : 0);

    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    unsigned char symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    for (unsigned int i = 0; i < 48; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (!frame.AddrPC.Offset) break;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
            std::fprintf(stderr, "[platform-win32] stack[%u]=0x%llX %s+0x%llX\n",
                         i, frame.AddrPC.Offset, symbol->Name, displacement);
        else
            std::fprintf(stderr, "[platform-win32] stack[%u]=0x%llX\n",
                         i, frame.AddrPC.Offset);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

struct CrashTraceInstaller
{
    CrashTraceInstaller()
    {
        const char* enabled = std::getenv("MC2_CRASH_TRACE");
        if (!enabled || !*enabled || std::strcmp(enabled, "0") == 0) return;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        AddVectoredExceptionHandler(1, CrashTrace);
        std::fprintf(stderr, "[platform-win32] crash trace enabled\n");
    }
};

CrashTraceInstaller g_crashTraceInstaller;

int MapVirtualKeyToGameOS(WPARAM key)
{
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
        (key >= VK_F1 && key <= VK_F15) ||
        (key >= VK_NUMPAD0 && key <= VK_NUMPAD9))
        return static_cast<int>(key);

    switch (key)
    {
        case VK_ESCAPE: case VK_BACK: case VK_TAB: case VK_RETURN:
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_SPACE:
        case VK_CAPITAL: case VK_MULTIPLY: case VK_ADD: case VK_SUBTRACT:
        case VK_DECIMAL: case VK_DIVIDE: case VK_SNAPSHOT: case VK_HOME:
        case VK_UP: case VK_PRIOR: case VK_LEFT: case VK_CLEAR: case VK_RIGHT:
        case VK_END: case VK_DOWN: case VK_NEXT: case VK_INSERT: case VK_DELETE:
        case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_NUMLOCK: case VK_PAUSE:
        case VK_OEM_MINUS: case VK_OEM_PLUS: case VK_OEM_4: case VK_OEM_6:
        case VK_OEM_1: case VK_OEM_7: case VK_OEM_3: case VK_OEM_5:
        case VK_OEM_COMMA: case VK_OEM_PERIOD: case VK_OEM_2:
            return static_cast<int>(key);
        case VK_LSHIFT: case VK_RSHIFT: return KEY_LSHIFT;
        case VK_LCONTROL: case VK_RCONTROL: return KEY_LCONTROL;
        case VK_LMENU: case VK_RMENU: return KEY_LMENU;
        default: return -1;
    }
}

struct WindowBounds
{
    int x, y, width, height;
};

WindowBounds FitWindowToWorkArea(HWND window, int clientWidth, int clientHeight,
                                 DWORD style, DWORD exStyle)
{
    HMONITOR monitor = nullptr;
    if (window)
        monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    else
    {
        const POINT origin = { 0, 0 };
        monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }

    MONITORINFO info = { sizeof(info) };
    GetMonitorInfoA(monitor, &info);
    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;

    RECT requested = { 0, 0, clientWidth, clientHeight };
    AdjustWindowRectEx(&requested, style, FALSE, exStyle);
    const int frameWidth = (requested.right - requested.left) - clientWidth;
    const int frameHeight = (requested.bottom - requested.top) - clientHeight;
    const int availableClientWidth = workWidth > frameWidth ? workWidth - frameWidth : 1;
    const int availableClientHeight = workHeight > frameHeight ? workHeight - frameHeight : 1;

    const double scaleX = static_cast<double>(availableClientWidth) / clientWidth;
    const double scaleY = static_cast<double>(availableClientHeight) / clientHeight;
    double scale = scaleX < scaleY ? scaleX : scaleY;
    if (scale > 1.0) scale = 1.0;
    const int fittedClientWidth = static_cast<int>(std::floor(clientWidth * scale));
    const int fittedClientHeight = static_cast<int>(std::floor(clientHeight * scale));

    RECT fitted = { 0, 0, fittedClientWidth, fittedClientHeight };
    AdjustWindowRectEx(&fitted, style, FALSE, exStyle);
    const int outerWidth = fitted.right - fitted.left;
    const int outerHeight = fitted.bottom - fitted.top;
    return {
        info.rcWork.left + (workWidth - outerWidth) / 2,
        info.rcWork.top + (workHeight - outerHeight) / 2,
        outerWidth,
        outerHeight
    };
}

class Win32Platform final : public PlatformBackend
{
public:
    Win32Platform()
        : m_instance(nullptr), m_window(nullptr), m_width(0), m_height(0),
          m_gameWidth(0), m_gameHeight(0),
          m_mouseX(0), m_mouseY(0), m_pendingDx(0), m_pendingDy(0),
          m_pendingWheel(0), m_buttons(0), m_lastKey(0), m_fullscreen(false),
          m_windowedStyle(0), m_pumpCount(0), m_capturePump(0), m_captured(false),
          m_resizePump(0), m_testResizeApplied(false),
          m_inputProof(false), m_mouseFocusCensus(false),
          m_mouseConfineRequested(false), m_cursorClipped(false),
          m_mouseEvents(0), m_keyDownEvents(0),
          m_keyUpEvents(0), m_charEvents(0), m_resizeEvents(0), m_closeEvents(0)
    {
        std::memset(&m_windowedPlacement, 0, sizeof(m_windowedPlacement));
        m_windowedPlacement.length = sizeof(m_windowedPlacement);
    }

    const char* Name() const override { return "win32"; }

    bool Create(const char* title, int clientWidth, int clientHeight) override
    {
        if (m_window) return true;
        m_instance = GetModuleHandleA(nullptr);
        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = &WindowProc;
        wc.hInstance = m_instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = "MC2NativePortWindow";
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::fprintf(stderr, "[platform-win32] RegisterClassEx failed: %lu\n",
                         static_cast<unsigned long>(GetLastError()));
            return false;
        }

        const DWORD style = WS_OVERLAPPEDWINDOW;
        const WindowBounds bounds = FitWindowToWorkArea(
            nullptr, clientWidth, clientHeight, style, 0);
        m_window = CreateWindowExA(0, wc.lpszClassName,
            title && *title ? title : "MechCommander 2", style,
            bounds.x, bounds.y, bounds.width, bounds.height,
            nullptr, nullptr, m_instance, this);
        if (!m_window)
        {
            std::fprintf(stderr, "[platform-win32] CreateWindowEx failed: %lu\n",
                         static_cast<unsigned long>(GetLastError()));
            return false;
        }
        hWindow = m_window;
        ShowWindow(m_window, SW_SHOW);
        UpdateWindow(m_window);
        UpdateGeometry();
        SetMouseConfined(true);
        m_gameWidth = m_width;
        m_gameHeight = m_height;
        const char* capture = std::getenv("MC2_WINDOW_CAPTURE_FRAME");
        if (capture && *capture) m_capturePump = std::strtoul(capture, nullptr, 10);
        // PORT (M8-B18): PREFER THIS. Anchors the shot to simulation progress
        // rather than to how long loading took. See mc2_port.h.
        const char* captureTurn = std::getenv("MC2_CAPTURE_TURN");
        if (captureTurn && *captureTurn)
            m_captureTurn = std::strtol(captureTurn, nullptr, 10);
        const char* resize = std::getenv("MC2_WINDOW_RESIZE_FRAME");
        if (resize && *resize) m_resizePump = std::strtoul(resize, nullptr, 10);
        const char* inputProof = std::getenv("MC2_INPUT_PROOF");
        m_inputProof = inputProof && *inputProof && std::strcmp(inputProof, "0") != 0;
        if (m_inputProof)
            std::printf("[platform-win32] input proof enabled: mouse move/click + right arrow\n");
        const char* mouseFocusCensus = std::getenv("MC2_MOUSE_FOCUS_CENSUS");
        m_mouseFocusCensus = mouseFocusCensus && *mouseFocusCensus &&
                             std::strcmp(mouseFocusCensus, "0") != 0;
        std::printf("[platform-win32] window created: client=%dx%d hwnd=%p\n",
                    m_width, m_height, static_cast<void*>(m_window));
        return true;
    }

    void Destroy() override
    {
        ReleaseMouseInput();
        if (m_window && IsWindow(m_window)) DestroyWindow(m_window);
        m_window = nullptr;
        hWindow = nullptr;
        clientToScreen.x = clientToScreen.y = 0;
    }

    bool PumpEvents() override
    {
        MSG message;
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                g_gosQuitRequested = true;
                return false;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (!m_window || g_gosQuitRequested) return false;
        const float scaleX = m_width > 0 && m_gameWidth > 0
            ? static_cast<float>(m_gameWidth) / static_cast<float>(m_width) : 1.0f;
        const float scaleY = m_height > 0 && m_gameHeight > 0
            ? static_cast<float>(m_gameHeight) / static_cast<float>(m_height) : 1.0f;
        // PORT: GameOS reports the mouse POSITION as a fraction of the window,
        // not in pixels -- "Position returns values converted into position on
        // window. Range is 0.0 to 1.0 (0 to 639 on a 640 wide screen)"
        // (Source/GameOS/include/GameOS.hpp, gos_GetMouseInfo). The game scales
        // it back up itself with the gos_GetViewport multipliers
        // (UserInput::getMouseX == mouseXPosition * viewMulX). Feeding pixels
        // here squared the coordinate: a click at client x=400 reached the GUI
        // as x=320000, so every pointInside() test in the interface failed and
        // no button in the game could ever be pressed.
        //
        // DELTAS stay in game-space pixels: the prototype types them int, and
        // the camera scales them by 0.01f (MLRClipper.cpp).
        const float normMouseX = m_width  > 0
            ? static_cast<float>(m_mouseX) / static_cast<float>(m_width)  : 0.0f;
        const float normMouseY = m_height > 0
            ? static_cast<float>(m_mouseY) / static_cast<float>(m_height) : 0.0f;
        const int gameDx = static_cast<int>(std::lround(static_cast<float>(m_pendingDx) * scaleX));
        const int gameDy = static_cast<int>(std::lround(static_cast<float>(m_pendingDy) * scaleY));
        gosInput_SetMouse(normMouseX, normMouseY, gameDx, gameDy,
                          m_pendingWheel, m_buttons);
        if (m_inputProof && (m_pendingDx || m_pendingDy || m_buttons))
            std::printf("[platform-win32] input proof delivered: client=%d,%d "
                        "clientSize=%dx%d gos=%.4f,%.4f game=%.1f,%.1f gameSize=%dx%d "
                        "buttons=0x%lX\n",
                        m_mouseX, m_mouseY, m_width, m_height,
                        normMouseX, normMouseY,
                        normMouseX * static_cast<float>(m_gameWidth),
                        normMouseY * static_cast<float>(m_gameHeight),
                        m_gameWidth, m_gameHeight,
                        static_cast<unsigned long>(m_buttons));
        m_pendingDx = m_pendingDy = m_pendingWheel = 0;
        UpdateGeometry();
        ++m_pumpCount;
        // PORT: Opt-in diagnostic posts genuine Win32 messages so the normal
        // WindowProc -> GameOS input path can be proven without test-only hooks.
        if (m_inputProof)
        {
            if (m_pumpCount == 750)
            {
                PostMessageA(m_window, WM_MOUSEMOVE, 0, MAKELPARAM(m_width / 2, m_height / 2));
                std::printf("[platform-win32] input proof posted mouse move at pump 750\n");
            }
            else if (m_pumpCount == 800)
            {
                PostMessageA(m_window, WM_KEYDOWN, VK_RIGHT, 1);
                std::printf("[platform-win32] input proof posted right-arrow down at pump 800\n");
            }
            else if (m_pumpCount == 1100)
            {
                PostMessageA(m_window, WM_KEYUP, VK_RIGHT, 0xC0000001L);
                std::printf("[platform-win32] input proof posted right-arrow up at pump 1100\n");
            }
            else if (m_pumpCount == 1200)
            {
                const LPARAM point = MAKELPARAM(400, 139);
                PostMessageA(m_window, WM_MOUSEMOVE, 0, point);
                PostMessageA(m_window, WM_LBUTTONDOWN, MK_LBUTTON, point);
                // PORT: UP is posted at pump 1260, not here -- see below.

                std::printf("[platform-win32] input proof posted HUD click at pump 1200\n");
            }
            else if (m_pumpCount == 1260)
            {
                // PORT: DOWN and UP must be separated by at least one pump. Posted
                // back to back they are drained by the same PeekMessage loop, so the
                // key goes FREE -> PRESSED -> RELEASED before DoGameLogic reads it
                // and the synthetic click can never be observed.
                const LPARAM point = MAKELPARAM(400, 139);
                PostMessageA(m_window, WM_LBUTTONUP, 0, point);
                std::printf("[platform-win32] input proof posted click UP at pump 1260\n");
            }
        }
        if (!m_testResizeApplied && m_resizePump && m_pumpCount >= m_resizePump)
        {
            RECT bounds = { 0, 0, 1000, 700 };
            const DWORD style = static_cast<DWORD>(GetWindowLongPtrA(m_window, GWL_STYLE));
            AdjustWindowRectEx(&bounds, style, FALSE, 0);
            SetWindowPos(m_window, nullptr, 0, 0, bounds.right - bounds.left,
                         bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOZORDER);
            m_testResizeApplied = true;
            std::printf("[platform-win32] diagnostic resize requested: client=1000x700\n");
        }
        // MC2_CAPTURE_TURN wins when both are set: it is the reproducible one.
        if (!m_captured && m_captureTurn > 0)
        {
            if (MC2_PublishedTurn >= m_captureTurn) m_captured = CaptureWindow();
        }
        else if (!m_captured && m_capturePump && m_pumpCount >= m_capturePump)
            m_captured = CaptureWindow();
        return true;
    }

    bool HasWindow() const override { return m_window && IsWindow(m_window); }
    void* NativeWindowHandle() const override { return m_window; }
    void* NativeDisplayHandle() const override { return m_instance; }
    void ClientSize(int* width, int* height) const override
    { if (width) *width = m_width; if (height) *height = m_height; }
    void ClientScreenOffset(long* x, long* y) const override
    { if (x) *x = clientToScreen.x; if (y) *y = clientToScreen.y; }

    void SetGameScreenSize(int width, int height) override
    {
        if (width <= 0 || height <= 0 ||
            (width == m_gameWidth && height == m_gameHeight)) return;
        m_gameWidth = width;
        m_gameHeight = height;
        if (!m_window || m_fullscreen) return;

        const DWORD style = static_cast<DWORD>(GetWindowLongPtrA(m_window, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrA(m_window, GWL_EXSTYLE));
        // PORT: a client as large as the monitor plus non-client borders cannot
        // fit on screen. Preserve the game resolution internally, fit the
        // visible window to the monitor work area, and centre it.
        const WindowBounds bounds = FitWindowToWorkArea(
            m_window, width, height, style, exStyle);
        SetWindowPos(m_window, nullptr, bounds.x, bounds.y,
                     bounds.width, bounds.height, SWP_NOZORDER);
        UpdateGeometry();
        std::printf("[platform-win32] game screen changed: game=%dx%d client=%dx%d\n",
                    m_gameWidth, m_gameHeight, m_width, m_height);
    }

    void SetFullscreen(bool fullscreen) override
    {
        if (!m_window || fullscreen == m_fullscreen) return;
        if (fullscreen)
        {
            m_windowedStyle = static_cast<DWORD>(GetWindowLongPtrA(m_window, GWL_STYLE));
            GetWindowPlacement(m_window, &m_windowedPlacement);
            MONITORINFO monitor = { sizeof(monitor) };
            GetMonitorInfoA(MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrA(m_window, GWL_STYLE,
                              m_windowedStyle & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
            SetWindowPos(m_window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                         monitor.rcMonitor.right - monitor.rcMonitor.left,
                         monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        else
        {
            SetWindowLongPtrA(m_window, GWL_STYLE, m_windowedStyle);
            SetWindowPlacement(m_window, &m_windowedPlacement);
            SetWindowPos(m_window, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        m_fullscreen = fullscreen;
        std::printf("[platform-win32] fullscreen=%s\n", fullscreen ? "yes" : "no");
    }

    void SetMouseConfined(bool confined) override
    {
        m_mouseConfineRequested = confined;
        if (!confined) ReleaseMouseInput();
        else UpdateMouseClip();
        TraceMouseFocus(confined ? "CONFINE_ON" : "CONFINE_OFF");
    }

    void DumpStats() const override
    {
        std::printf("[platform-win32] events: mouse=%lu keyDown=%lu keyUp=%lu chars=%lu "
                    "resizes=%lu closes=%lu lastMouse=%d,%d client=%dx%d\n",
                    m_mouseEvents, m_keyDownEvents, m_keyUpEvents, m_charEvents,
                    m_resizeEvents, m_closeEvents, m_mouseX, m_mouseY, m_width, m_height);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Win32Platform* self = reinterpret_cast<Win32Platform*>(
            GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            CREATESTRUCTA* create = reinterpret_cast<CREATESTRUCTA*>(lParam);
            self = static_cast<Win32Platform*>(create->lpCreateParams);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->HandleMessage(hwnd, message, wParam, lParam) :
                      DefWindowProcA(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
            case WM_CLOSE:
                ++m_closeEvents;
                g_gosQuitRequested = true;
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                m_window = nullptr;
                hWindow = nullptr;
                PostQuitMessage(0);
                return 0;
            case WM_SIZE:
                m_width = LOWORD(lParam); m_height = HIWORD(lParam);
                if (m_width > 0 && m_height > 0) ++m_resizeEvents;
                UpdateMouseClip();
                return 0;
            case WM_MOVE:
                UpdateGeometry();
                UpdateMouseClip();
                return 0;
            case WM_MOUSEMOVE:
            {
                const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
                m_pendingDx += x - m_mouseX; m_pendingDy += y - m_mouseY;
                m_mouseX = x; m_mouseY = y; ++m_mouseEvents;
                return 0;
            }
            case WM_MOUSEWHEEL:
                m_pendingWheel += GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                return 0;
            case WM_LBUTTONDOWN: TraceMouseFocus("WM_LBUTTONDOWN"); SetMouseButton(KEY_LMOUSE, 0x01, true); return 0;
            case WM_LBUTTONUP:   TraceMouseFocus("WM_LBUTTONUP");   SetMouseButton(KEY_LMOUSE, 0x01, false); return 0;
            case WM_RBUTTONDOWN: TraceMouseFocus("WM_RBUTTONDOWN"); SetMouseButton(KEY_RMOUSE, 0x02, true); return 0;
            case WM_RBUTTONUP:   TraceMouseFocus("WM_RBUTTONUP");   SetMouseButton(KEY_RMOUSE, 0x02, false); return 0;
            case WM_MBUTTONDOWN: TraceMouseFocus("WM_MBUTTONDOWN"); SetMouseButton(KEY_MMOUSE, 0x04, true); return 0;
            case WM_MBUTTONUP:   TraceMouseFocus("WM_MBUTTONUP");   SetMouseButton(KEY_MMOUSE, 0x04, false); return 0;
            case WM_XBUTTONDOWN:
                SetMouseButton(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? KEY_MOUSEX1 : KEY_MOUSEX2,
                               GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 0x08 : 0x10, true);
                return TRUE;
            case WM_XBUTTONUP:
                SetMouseButton(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? KEY_MOUSEX1 : KEY_MOUSEX2,
                               GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 0x08 : 0x10, false);
                return TRUE;
            case WM_KEYDOWN: case WM_SYSKEYDOWN:
            {
                const int key = MapVirtualKeyToGameOS(wParam);
                if (key >= 0)
                {
                    gosInput_SetKey(key, true); m_lastKey = key; ++m_keyDownEvents;
                }
                if (message == WM_SYSKEYDOWN && (wParam == VK_SPACE || wParam == VK_F4))
                    return DefWindowProcA(hwnd, message, wParam, lParam);
                return 0;
            }
            case WM_KEYUP: case WM_SYSKEYUP:
            {
                const int key = MapVirtualKeyToGameOS(wParam);
                if (key >= 0)
                {
                    gosInput_SetKey(key, false); ++m_keyUpEvents;
                }
                return 0;
            }
            case WM_CHAR:
                gosInput_PushKeyEvent((static_cast<unsigned long>(m_lastKey) << 8) |
                                      (static_cast<unsigned long>(wParam) & 0xFF));
                ++m_charEvents;
                return 0;
            case WM_SETFOCUS:
                TraceMouseFocus("WM_SETFOCUS");
                UpdateMouseClip();
                return 0;
            case WM_KILLFOCUS:
                TraceMouseFocus("WM_KILLFOCUS");
                for (int key = 0; key < 256; ++key) gosInput_SetKey(key, false);
                ReleaseMouseInput();
                return 0;
            case WM_CAPTURECHANGED:
                if (reinterpret_cast<HWND>(lParam) != m_window && m_buttons)
                    ClearMouseButtons();
                return 0;
            default: return DefWindowProcA(hwnd, message, wParam, lParam);
        }
    }

    void SetMouseButton(int key, DWORD mask, bool down)
    {
        if (down && GetFocus() != m_window) return;
        if (down) { m_buttons |= mask; SetCapture(m_window); }
        else { m_buttons &= ~mask; if (!m_buttons) ReleaseCapture(); }
        gosInput_SetKey(key, down);
    }

    void ClearMouseButtons()
    {
        m_buttons = 0;
        gosInput_SetKey(KEY_LMOUSE, false);
        gosInput_SetKey(KEY_RMOUSE, false);
        gosInput_SetKey(KEY_MMOUSE, false);
        gosInput_SetKey(KEY_MOUSEX1, false);
        gosInput_SetKey(KEY_MOUSEX2, false);
    }

    void ReleaseMouseInput()
    {
        ClearMouseButtons();
        if (m_window && GetCapture() == m_window) ReleaseCapture();
        if (m_cursorClipped)
        {
            ClipCursor(nullptr);
            m_cursorClipped = false;
        }
    }

    void UpdateMouseClip()
    {
        if (!m_window || !m_mouseConfineRequested || GetFocus() != m_window)
        {
            if (m_cursorClipped)
            {
                ClipCursor(nullptr);
                m_cursorClipped = false;
            }
            return;
        }
        RECT client = {};
        if (!GetClientRect(m_window, &client)) return;
        POINT topLeft = { client.left, client.top };
        POINT bottomRight = { client.right, client.bottom };
        if (!ClientToScreen(m_window, &topLeft) ||
            !ClientToScreen(m_window, &bottomRight)) return;
        const RECT screenClient = {
            topLeft.x, topLeft.y, bottomRight.x, bottomRight.y
        };
        m_cursorClipped = ClipCursor(&screenClient) != FALSE;
    }

    void TraceMouseFocus(const char* event) const
    {
        if (!m_mouseFocusCensus) return;
        std::fprintf(stderr,
                     "[mouse-focus] event=%s focus=%p window=%p capture=%p "
                     "buttons=0x%lX requested=%d clipped=%d\n",
                     event, static_cast<void*>(GetFocus()), static_cast<void*>(m_window),
                     static_cast<void*>(GetCapture()), static_cast<unsigned long>(m_buttons),
                     m_mouseConfineRequested ? 1 : 0, m_cursorClipped ? 1 : 0);
        std::fflush(stderr);
    }

    void UpdateGeometry()
    {
        if (!m_window) return;
        RECT client;
        if (GetClientRect(m_window, &client))
        { m_width = client.right - client.left; m_height = client.bottom - client.top; }
        POINT origin = { 0, 0 };
        if (ClientToScreen(m_window, &origin)) clientToScreen = origin;
    }

    bool CaptureWindow() const
    {
        RECT bounds;
        if (!GetWindowRect(m_window, &bounds)) return false;
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        const bool copied = BitBlt(memory, 0, 0, width, height, screen,
                                  bounds.left, bounds.top, SRCCOPY | CAPTUREBLT) != FALSE;
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        unsigned char* pixels = nullptr;
        HBITMAP dib = CreateDIBSection(memory, &info, DIB_RGB_COLORS,
                                       reinterpret_cast<void**>(&pixels), nullptr, 0);
        HGDIOBJ oldDib = SelectObject(memory, dib);
        if (copied) BitBlt(memory, 0, 0, width, height, screen,
                           bounds.left, bounds.top, SRCCOPY | CAPTUREBLT);

        bool written = false;
        FILE* output = pixels ? std::fopen("mc2-window.ppm", "wb") : nullptr;
        if (output)
        {
            std::fprintf(output, "P6\n%d %d\n255\n", width, height);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const unsigned char* bgra = pixels + (y * width + x) * 4;
                    const unsigned char rgb[3] = { bgra[2], bgra[1], bgra[0] };
                    std::fwrite(rgb, 1, 3, output);
                }
            std::fclose(output);
            written = true;
            std::printf("[platform-win32] captured visible window to mc2-window.ppm (%dx%d)\n",
                        width, height);
        }
        SelectObject(memory, oldDib);
        DeleteObject(dib);
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return written;
    }

    HINSTANCE m_instance;
    HWND m_window;
    int m_width, m_height;
    int m_gameWidth, m_gameHeight;
    int m_mouseX, m_mouseY, m_pendingDx, m_pendingDy, m_pendingWheel;
    DWORD m_buttons;
    int m_lastKey;
    bool m_fullscreen;
    DWORD m_windowedStyle;
    WINDOWPLACEMENT m_windowedPlacement;
    unsigned long m_pumpCount, m_capturePump;
    long m_captureTurn = 0;   // PORT (M8-B18)
    bool m_captured;
    unsigned long m_resizePump;
    bool m_testResizeApplied;
    bool m_inputProof;
    bool m_mouseFocusCensus;
    bool m_mouseConfineRequested;
    bool m_cursorClipped;
    unsigned long m_mouseEvents, m_keyDownEvents, m_keyUpEvents, m_charEvents;
    unsigned long m_resizeEvents, m_closeEvents;
};

} // namespace

PlatformBackend* CreateWin32PlatformBackend()
{
    static Win32Platform backend;
    // PORT: hand gos_Info_ValidMode a way to bound the resolution list by the
    // real display. Registered here rather than called from gos_system.cpp,
    // which must not reference this library -- see gos_backend.h.
    MC2_SetDisplaySizeProvider(&MC2_PlatformDisplaySize);
    return &backend;
}

void __stdcall gos_SetMouseConfined(bool confined)
{
    CreateWin32PlatformBackend()->SetMouseConfined(confined);
}

// PORT: primary display size, for honest gos_Info_ValidMode answers. Contract is
// documented on the declaration in platform/platform_backend.h.
//
// SM_CXSCREEN/SM_CYSCREEN rather than the monitor the window happens to be on:
// the resolution list is built before the player has moved anything, and a mode
// list that changed when the window was dragged between monitors would be worse
// than one that is merely primary-relative.
bool MC2_PlatformDisplaySize(int* width, int* height)
{
    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return false;
    if (width)  *width  = w;
    if (height) *height = h;
    return true;
}

// PORT: host-heap validation for the teardown census. The contract is
// documented on the declaration in platform/platform_backend.h.
//
// EVERY heap in the process is walked, not just GetProcessHeap(): the UCRT's
// malloc uses the process heap, but dbghelp, the graphics runtime and the audio
// device each create private heaps, and an overrun in one of those is just as
// fatal. GetProcessHeaps is the only way to enumerate them.
//
// HeapValidate takes each heap's lock, so this is safe while other threads
// allocate -- but it is a full walk of every block in the process, so it is
// only ever reached from the env-gated checkpoint, never from a frame path.
int MC2_PlatformHeapIntact()
{
    HANDLE heaps[128];
    const DWORD capacity = static_cast<DWORD>(sizeof(heaps) / sizeof(heaps[0]));
    const DWORD count = GetProcessHeaps(capacity, heaps);
    if (count == 0) return -1;

    const DWORD walked = (count > capacity) ? capacity : count;

    int intact = 1;
    for (DWORD i = 0; i < walked; ++i)
    {
        if (!HeapValidate(heaps[i], 0, NULL))
        {
            std::fprintf(stderr, "[teardown]   HeapValidate FAILED on heap %u of %u (%p)%s\n",
                         static_cast<unsigned>(i + 1), static_cast<unsigned>(walked), heaps[i],
                         heaps[i] == GetProcessHeap() ? "  <== the process heap, which is the CRT's" : "");
            std::fflush(stderr);
            intact = 0;
        }
    }
    return intact;
}
