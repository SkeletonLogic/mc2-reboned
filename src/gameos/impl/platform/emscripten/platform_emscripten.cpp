//===========================================================================//
// Emscripten platform peer: canvas geometry, browser input, and timers.
//===========================================================================//

#include "../platform_backend.h"
#include "../../gos_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>
#include <gameos.hpp>
#include <mmsystem.h>
#include <unordered_map>

namespace {

EM_JS(double, mc2CanvasLeft, (), {
    const canvas = document.getElementById('canvas');
    return canvas ? canvas.getBoundingClientRect().left : 0;
});

EM_JS(double, mc2CanvasTop, (), {
    const canvas = document.getElementById('canvas');
    return canvas ? canvas.getBoundingClientRect().top : 0;
});

EM_JS(int, mc2PointerLocked, (), {
    const canvas = document.getElementById('canvas');
    return (canvas && document.pointerLockElement === canvas) ? 1 : 0;
});

class EmscriptenPlatform final : public PlatformBackend
{
public:
    EmscriptenPlatform()
        : m_width(0), m_height(0), m_cssWidth(0.0), m_cssHeight(0.0),
          m_mouseX(0), m_mouseY(0), m_pendingDx(0), m_pendingDy(0),
          m_pendingWheel(0), m_buttons(0), m_lastKey(0), m_pumps(0),
          m_mouseEvents(0), m_keyEvents(0), m_charEvents(0), m_created(false), m_inputProof(false) {}

    const char* Name() const override { return "emscripten-canvas"; }
    bool Create(const char*, int width, int height) override
    {
        m_width = width;
        m_height = height;
        updateCanvasSize();
        m_inputProof = enabled("MC2_INPUT_PROOF");
        emscripten_set_mousemove_callback("#canvas", this, true, mouseCallback);
        emscripten_set_mousedown_callback("#canvas", this, true, mouseCallback);
        emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, mouseCallback);
        emscripten_set_wheel_callback("#canvas", this, true, wheelCallback);
        emscripten_set_keydown_callback("#canvas", this, true, keyboardCallback);
        emscripten_set_keyup_callback("#canvas", this, true, keyboardCallback);
        emscripten_set_keypress_callback("#canvas", this, true, keyboardCallback);
        emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, blurCallback);
        emscripten_set_blur_callback("#canvas", this, true, blurCallback);
        m_created = true;
        std::printf("[platform-emscripten] canvas input ready: client=%.0fx%.0f game=%dx%d\n",
                    m_cssWidth, m_cssHeight, m_width, m_height);
        return true;
    }
    void Destroy() override
    {
        emscripten_set_mousemove_callback("#canvas", nullptr, true, nullptr);
        emscripten_set_mousedown_callback("#canvas", nullptr, true, nullptr);
        emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, nullptr);
        emscripten_set_wheel_callback("#canvas", nullptr, true, nullptr);
        emscripten_set_keydown_callback("#canvas", nullptr, true, nullptr);
        emscripten_set_keyup_callback("#canvas", nullptr, true, nullptr);
        emscripten_set_keypress_callback("#canvas", nullptr, true, nullptr);
        emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, nullptr);
        emscripten_set_blur_callback("#canvas", nullptr, true, nullptr);
        m_created = false;
    }
    bool PumpEvents() override
    {
        updateCanvasSize();
        bool delivered[256] = {};
        for (auto event = m_keyTransitions.begin(); event != m_keyTransitions.end();)
        {
            const int key = event->key;
            if (delivered[key]) { ++event; continue; }
            gosInput_SetKey(key, event->down);
            if (key >= KEY_LMOUSE && key <= KEY_MOUSEX2)
            {
                const unsigned long mask = 1ul << (key - KEY_LMOUSE);
                if (event->down) m_buttons |= mask;
                else m_buttons &= ~mask;
            }
            delivered[key] = true;
            event = m_keyTransitions.erase(event);
        }

        const double clientX = std::max(0.0, std::min(m_cssWidth, static_cast<double>(m_mouseX)));
        const double clientY = std::max(0.0, std::min(m_cssHeight, static_cast<double>(m_mouseY)));
        const float normX = m_cssWidth > 0.0
            ? static_cast<float>(clientX / m_cssWidth) : 0.0f;
        const float normY = m_cssHeight > 0.0
            ? static_cast<float>(clientY / m_cssHeight) : 0.0f;
        const int gameDx = m_cssWidth > 0.0
            ? static_cast<int>(std::lround(m_pendingDx * m_width / m_cssWidth)) : 0;
        const int gameDy = m_cssHeight > 0.0
            ? static_cast<int>(std::lround(m_pendingDy * m_height / m_cssHeight)) : 0;
        gosInput_SetMouse(normX, normY, gameDx, gameDy, m_pendingWheel, m_buttons);
        if (m_inputProof && (m_pendingDx || m_pendingDy || m_buttons))
            std::printf("[platform-emscripten] input proof delivered: client=%d,%d "
                        "clientSize=%.0fx%.0f gos=%.4f,%.4f game=%.1f,%.1f "
                        "gameSize=%dx%d buttons=0x%lX\n",
                        m_mouseX, m_mouseY, m_cssWidth, m_cssHeight, normX, normY,
                        normX * m_width, normY * m_height, m_width, m_height, m_buttons);
        m_pendingDx = m_pendingDy = m_pendingWheel = 0;
        ++m_pumps;
        return true;
    }
    bool HasWindow() const override { return m_created; }
    void* NativeWindowHandle() const override { return nullptr; }
    void* NativeDisplayHandle() const override { return nullptr; }
    void ClientSize(int* width, int* height) const override
    {
        if (width) *width = m_cssWidth > 0.0 ? static_cast<int>(std::lround(m_cssWidth)) : m_width;
        if (height) *height = m_cssHeight > 0.0 ? static_cast<int>(std::lround(m_cssHeight)) : m_height;
    }
    void ClientScreenOffset(long* x, long* y) const override
    {
        if (x) *x = 0;
        if (y) *y = 0;
    }
    void SetGameScreenSize(int width, int height) override
    {
        m_width = width;
        m_height = height;
    }
    void SetFullscreen(bool) override {}
    void SetMouseConfined(bool) override {}
    void DumpStats() const override
    {
        std::printf("[platform-emscripten] pumps=%lu canvas=yes client=%.0fx%.0f "
                    "game=%dx%d mouseEvents=%lu keyEvents=%lu charEvents=%lu\n",
                    m_pumps, m_cssWidth, m_cssHeight, m_width, m_height,
                    m_mouseEvents, m_keyEvents, m_charEvents);
    }

private:
    struct KeyTransition { int key; bool down; };

    static bool enabled(const char* name)
    {
        const char* value = std::getenv(name);
        return value && *value && std::strcmp(value, "0") != 0;
    }

    void updateCanvasSize()
    {
        double width = 0.0, height = 0.0;
        if (emscripten_get_element_css_size("#canvas", &width, &height) == EMSCRIPTEN_RESULT_SUCCESS &&
            width > 0.0 && height > 0.0)
        {
            m_cssWidth = width;
            m_cssHeight = height;
        }
    }

    static int mapKeyboard(const EmscriptenKeyboardEvent* event)
    {
        const int key = static_cast<int>(event->keyCode);
        return key >= 0 && key <= 255 ? key : -1;
    }

    static EM_BOOL mouseCallback(int eventType, const EmscriptenMouseEvent* event, void* userData)
    {
        auto* self = static_cast<EmscriptenPlatform*>(userData);
        if (!self || !event) return EM_FALSE;
        self->updateCanvasSize();
        // PORT: fullscreen on a multi-monitor desktop needs POINTER LOCK, or the
        // pointer walks straight off the game onto the second screen -- the owner
        // hit exactly that. While locked the browser stops updating clientX/clientY
        // (they freeze at the lock point) and only movementX/movementY carry
        // information, so the cursor position has to be integrated here and clamped
        // to the canvas. Unlocked, keep the original absolute mapping.
        if (mc2PointerLocked())
        {
            self->m_mouseX += static_cast<int>(std::lround(event->movementX));
            self->m_mouseY += static_cast<int>(std::lround(event->movementY));
            const int maxX = self->m_cssWidth  > 0.0 ? static_cast<int>(self->m_cssWidth)  - 1 : 0;
            const int maxY = self->m_cssHeight > 0.0 ? static_cast<int>(self->m_cssHeight) - 1 : 0;
            self->m_mouseX = std::max(0, std::min(maxX, self->m_mouseX));
            self->m_mouseY = std::max(0, std::min(maxY, self->m_mouseY));
        }
        else
        {
            // Emscripten's targetX/targetY are viewport-relative in this shell.
            // PORT: subtract the canvas bounds before normalizing; otherwise page
            // margins/header offsets shift every game-space hit test.
            self->m_mouseX = static_cast<int>(std::lround(event->clientX - mc2CanvasLeft()));
            self->m_mouseY = static_cast<int>(std::lround(event->clientY - mc2CanvasTop()));
        }
        self->m_pendingDx += event->movementX;
        self->m_pendingDy += event->movementY;
        ++self->m_mouseEvents;
        if (eventType == EMSCRIPTEN_EVENT_MOUSEDOWN)
            emscripten_run_script("document.getElementById('canvas').focus()");
        if (eventType == EMSCRIPTEN_EVENT_MOUSEDOWN || eventType == EMSCRIPTEN_EVENT_MOUSEUP)
        {
            int key = -1;
            if (event->button == 0) key = KEY_LMOUSE;
            else if (event->button == 1) key = KEY_MMOUSE;
            else if (event->button == 2) key = KEY_RMOUSE;
            else if (event->button == 3) key = KEY_MOUSEX1;
            else if (event->button == 4) key = KEY_MOUSEX2;
            if (key >= 0)
                self->m_keyTransitions.push_back({key, eventType == EMSCRIPTEN_EVENT_MOUSEDOWN});
        }
        return EM_TRUE;
    }

    static EM_BOOL wheelCallback(int, const EmscriptenWheelEvent* event, void* userData)
    {
        auto* self = static_cast<EmscriptenPlatform*>(userData);
        if (!self || !event) return EM_FALSE;
        self->m_pendingWheel += event->deltaY < 0.0 ? 1 : (event->deltaY > 0.0 ? -1 : 0);
        ++self->m_mouseEvents;
        return EM_TRUE;
    }

    // PORT (M8-B84): Alt-Tab away and the browser never delivers Alt's keyup
    // -- the canvas has lost focus by then -- so the engine kept Alt HELD and
    // every unmodified hotkey (Space, Esc, P, J) stopped matching. MEASURED over
    // CDP: P pauses and unpauses; after an Alt keydown with no keyup, P does
    // nothing. Desktop clears every key on WM_KILLFOCUS; this is that.
    static EM_BOOL blurCallback(int, const EmscriptenFocusEvent*, void* userData)
    {
        auto* self = static_cast<EmscriptenPlatform*>(userData);
        if (!self) return EM_FALSE;
        for (int key = 0; key < 256; ++key)
            if (self->m_keyHeld[key])
            {
                self->m_keyTransitions.push_back({key, false});
                self->m_keyHeld[key] = false;
            }
        return EM_FALSE;
    }

    static EM_BOOL keyboardCallback(int eventType, const EmscriptenKeyboardEvent* event,
                                    void* userData)
    {
        auto* self = static_cast<EmscriptenPlatform*>(userData);
        if (!self || !event) return EM_FALSE;
        if (eventType == EMSCRIPTEN_EVENT_KEYDOWN || eventType == EMSCRIPTEN_EVENT_KEYUP)
        {
            const int key = mapKeyboard(event);
            if (key >= 0)
            {
                self->m_keyTransitions.push_back({key, eventType == EMSCRIPTEN_EVENT_KEYDOWN});
                self->m_keyHeld[key] = (eventType == EMSCRIPTEN_EVENT_KEYDOWN);
                if (eventType == EMSCRIPTEN_EVENT_KEYDOWN) self->m_lastKey = key;
                ++self->m_keyEvents;
            }
            // PORT (M8-B82): the typed character comes from KEYDOWN, not KEYPRESS.
            // Returning EM_TRUE below cancels the keydown, and a cancelled keydown
            // means the browser never fires keypress -- so no character ever
            // reached gos_GetKey and every text box (variant name, save name)
            // stayed empty. Measured over CDP: canvas saw keydown/keyup only.
            // These are the characters WM_CHAR delivers on desktop.
            if (eventType == EMSCRIPTEN_EVENT_KEYDOWN && !event->ctrlKey && !event->altKey && !event->metaKey)
            {
                unsigned long character = 0;
                if (event->key[0] && !event->key[1])
                    character = static_cast<unsigned char>(event->key[0]);
                else if (!strcmp(event->key, "Backspace")) character = 0x08;
                else if (!strcmp(event->key, "Tab"))       character = 0x09;
                else if (!strcmp(event->key, "Enter"))     character = 0x0D;
                else if (!strcmp(event->key, "Escape"))    character = 0x1B;
                if (character)
                {
                    gosInput_PushKeyEvent((static_cast<unsigned long>(key >= 0 ? key : self->m_lastKey) << 8) |
                                          (character & 0xFF));
                    ++self->m_charEvents;
                }
            }
        }
        return EM_TRUE;
    }

    int m_width;
    int m_height;
    double m_cssWidth;
    double m_cssHeight;
    int m_mouseX;
    int m_mouseY;
    int m_pendingDx;
    int m_pendingDy;
    int m_pendingWheel;
    unsigned long m_buttons;
    int m_lastKey;
    std::deque<KeyTransition> m_keyTransitions;
    unsigned long m_pumps;
    unsigned long m_mouseEvents;
    unsigned long m_keyEvents;
    unsigned long m_charEvents;
    bool m_keyHeld[256] = {};
    bool m_created;
    bool m_inputProof;
};

} // namespace

namespace {
struct MultimediaTimer
{
    LPTIMECALLBACK callback;
    DWORD user;
};

std::unordered_map<int, MultimediaTimer*>& multimediaTimers()
{
    static std::unordered_map<int, MultimediaTimer*> timers;
    return timers;
}

void multimediaTimerThunk(void* context)
{
    auto* timer = static_cast<MultimediaTimer*>(context);
    timer->callback(0, 0, timer->user, 0, 0);
}
} // namespace

extern "C" DWORD timeGetTime()
{
    return static_cast<DWORD>(emscripten_get_now());
}

extern "C" MMRESULT timeSetEvent(UINT delay, UINT, LPTIMECALLBACK callback,
                                  DWORD user, UINT)
{
    if (!callback || delay == 0) return 0;
    auto* timer = new MultimediaTimer{callback, user};
    const int id = emscripten_set_interval(multimediaTimerThunk, delay, timer);
    if (id <= 0) {
        delete timer;
        return 0;
    }
    multimediaTimers()[id] = timer;
    return static_cast<MMRESULT>(id);
}

extern "C" MMRESULT timeKillEvent(UINT timerId)
{
    auto& timers = multimediaTimers();
    const auto found = timers.find(static_cast<int>(timerId));
    if (found == timers.end()) return 1;
    emscripten_clear_interval(static_cast<int>(timerId));
    delete found->second;
    timers.erase(found);
    return 0;
}

PlatformBackend* CreateEmscriptenPlatformBackend()
{
    static EmscriptenPlatform backend;
    return &backend;
}

void __stdcall gos_SetMouseConfined(bool confined)
{
    CreateEmscriptenPlatformBackend()->SetMouseConfined(confined);
}

// PORT: dlmalloc under Emscripten exposes no validation entry point, so the
// browser build answers "cannot tell" rather than a misleading "intact". The
// teardown census still prints its GameOS-side guard count there.
int MC2_PlatformHeapIntact()
{
    return -1;
}

// PORT: the browser has a canvas, not a mode list, and the shell owns its size.
// Answering "cannot say" makes gos_Info_ValidMode accept every mode, which is
// the behaviour the web build has always had.
bool MC2_PlatformDisplaySize(int* /*width*/, int* /*height*/)
{
    return false;
}
