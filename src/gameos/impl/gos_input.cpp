//===========================================================================//
// GameOS input, IME stubs, and the resource-string table.
//
// Clean-room reimplementation from Source/GameOS/include/GameOS.HPP.
//
// INPUT is real state that a windowing backend feeds. Until SDL2 is wired in
// there is no event source, so the queries report "nothing pressed, mouse at
// the origin" -- a quiescent state the game already handles (it is what an
// unfocused window looks like). The backend seam is deliberately narrow:
// gosInput_* below is everything SDL2 will need to drive.
//
// IME is stubbed on purpose. The port plan says do not implement East Asian
// input, and these seven entry points exist only so the link resolves.
//
// RESOURCE STRINGS come from MC2RES.DLL, a Win32 string table. The port cannot
// use LoadString (it is not portable and the DLL is retail-only), so this reads
// an extracted table instead -- see the note on gos_OpenResourceDLL.
//===========================================================================//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream> // PORT (M8-B10): records are read whole, not through a fixed buffer.
#include <map>
#include <string>

#include <gameos.hpp>

#include "gos_backend.h"
#include "platform/platform_backend.h" // PORT (M8-5 caller trace): MC2_STRPROBE stack.

namespace {

//---------------------------------------------------------------------------//
// Input state, owned here and pushed in by the windowing backend.
//---------------------------------------------------------------------------//
struct InputState
{
    float x, y;
    int   dx, dy, wheel;
    DWORD buttons;
    unsigned char keys[256];      // gosEnum_KeyIndex -> gosEnum_KeyStatus
    DWORD keyQueue[64];
    int   queueHead, queueTail;
};

InputState g_input = {};

} // namespace

//---------------------------------------------------------------------------//
// Backend feed. The SDL2 layer calls these; nothing else does. Keeping them
// out of the gos_ namespace makes it obvious they are not part of the GameOS
// API the game sees.
//---------------------------------------------------------------------------//
// PORT: x and y are a FRACTION OF THE WINDOW, 0.0 to 1.0 -- the range
// gos_GetMouseInfo is documented to return, and the range gos_SetMousePosition
// is given back (UserInput::setMousePos divides by the viewport multiplier
// before calling it). The game multiplies by the gos_GetViewport scalars to get
// pixels, so a backend that feeds pixels here squares every GUI coordinate.
// Deltas are game-space pixels; the prototype types them int.
void gosInput_SetMouse(float x, float y, int dx, int dy, int wheel, DWORD buttons)
{
    g_input.x = x; g_input.y = y;
    g_input.dx = dx; g_input.dy = dy;
    g_input.wheel = wheel;
    g_input.buttons = buttons;
}

void gosInput_BeginFrame()
{
    for (int key = 0; key < 256; ++key)
    {
        if (g_input.keys[key] == KEY_PRESSED) g_input.keys[key] = KEY_HELD;
        else if (g_input.keys[key] == KEY_RELEASED) g_input.keys[key] = KEY_FREE;
    }
}

void gosInput_SetKey(int keyIndex, bool down)
{
    if (keyIndex < 0 || keyIndex > 255) return;
    const gosEnum_KeyStatus current =
        static_cast<gosEnum_KeyStatus>(g_input.keys[keyIndex]);
    if (down)
    {
        if (current == KEY_FREE || current == KEY_RELEASED)
            g_input.keys[keyIndex] = KEY_PRESSED;
    }
    else if (current == KEY_PRESSED || current == KEY_HELD)
        g_input.keys[keyIndex] = KEY_RELEASED;
}

void gosInput_PushKeyEvent(DWORD key)
{
    int next = (g_input.queueTail + 1) % 64;
    if (next == g_input.queueHead) return;      // full; drop oldest-first policy
    g_input.keyQueue[g_input.queueTail] = key;
    g_input.queueTail = next;
}

//---------------------------------------------------------------------------//
// Keyboard
//---------------------------------------------------------------------------//
gosEnum_KeyStatus __stdcall gos_GetKeyStatus(gosEnum_KeyIndex index)
{
    const int i = (int)index;
    if (i < 0 || i > 255) return KEY_FREE;
    return static_cast<gosEnum_KeyStatus>(g_input.keys[i]);
}

DWORD __stdcall gos_GetKey()
{
    if (g_input.queueHead == g_input.queueTail) return 0;   // documented: 0 == no key
    DWORD k = g_input.keyQueue[g_input.queueHead];
    g_input.queueHead = (g_input.queueHead + 1) % 64;
    return k;
}

//---------------------------------------------------------------------------//
// Mouse
//---------------------------------------------------------------------------//
void __stdcall gos_GetMouseInfo(float* pXPosition, float* pYPosition,
                                int* pXDelta, int* pYDelta, int* pWheelDelta,
                                DWORD* pButtonsPressed)
{
    float gameX = g_input.x;
    float gameY = g_input.y;
    int gameDx = g_input.dx;
    int gameDy = g_input.dy;
    gosCanvas_PhysicalToGame(&gameX, &gameY, &gameDx, &gameDy);

    // PORT: The opt-in Win32 input harness reports at the public GameOS API,
    // proving the coordinates the game reads rather than only the messages posted.
    static float lastProofX = -1.0f, lastProofY = -1.0f;
    const char* inputProof = std::getenv("MC2_INPUT_PROOF");
    if (inputProof && *inputProof && std::strcmp(inputProof, "0") != 0 &&
        (gameX != lastProofX || gameY != lastProofY))
    {
        std::printf("[GameOS] input proof observed by gos_GetMouseInfo: gos=%.4f,%.4f "
                    "game=%.1f,%.1f\n", gameX, gameY,
                    gameX * (float)Environment.screenWidth,
                    gameY * (float)Environment.screenHeight);
        lastProofX = gameX;
        lastProofY = gameY;
    }
    if (pXPosition)     *pXPosition     = gameX;
    if (pYPosition)     *pYPosition     = gameY;
    if (pXDelta)        *pXDelta        = gameDx;
    if (pYDelta)        *pYDelta        = gameDy;
    if (pWheelDelta)    *pWheelDelta    = g_input.wheel;
    if (pButtonsPressed)*pButtonsPressed= g_input.buttons;

    // Deltas are consumed by reading, matching the original's per-frame
    // semantics -- otherwise the camera would keep drifting after the mouse stops.
    g_input.dx = g_input.dy = g_input.wheel = 0;
}

void __stdcall gos_SetMousePosition(float XPosition, float YPosition)
{
    gosCanvas_GameToPhysical(&XPosition, &YPosition);
    g_input.x = XPosition;
    g_input.y = YPosition;
}

//---------------------------------------------------------------------------//
// IME -- stubbed per the port plan. East Asian input is out of scope.
//---------------------------------------------------------------------------//
void _stdcall gos_EnableIME(bool /*enabledDisabled*/)              {}
void _stdcall gos_ToggleIME(bool /*activeInactive*/)               {}
void _stdcall gos_PositionIME(DWORD /*x*/, DWORD /*y*/)            {}
void _stdcall gos_FinalizeStringIME()                              {}
void _stdcall gos_SetIMELevel(DWORD /*dwImeLevel*/)                {}
void _stdcall gos_SetIMEAppearance(const gosIME_Appearance* /*pia*/) {}

void _stdcall gos_GetIMEAppearance(gosIME_Appearance* pia)
{
    if (pia) std::memset(pia, 0, sizeof(*pia));
}

//---------------------------------------------------------------------------//
// Resource strings
//
// The original loaded MC2RES.DLL and called LoadString. That is unavailable
// here: the DLL is retail content (not covered by the Shared Source licence, so
// it cannot ship with a web build) and LoadString is Win32-only.
//
// Instead a plain extracted table is read: "<id>\t<text>" per line, in a file
// named after the requested DLL with a .txt suffix. tools/iso-walk.mjs can pull
// MC2RES.DLL off the owner's disc for extraction; that is a development-time
// step, and only the extracted strings ever need to be redistributed.
//
// Until that table exists, lookups return a visible placeholder rather than an
// empty string, so missing text shows up on screen instead of silently
// vanishing.
//---------------------------------------------------------------------------//
namespace {

typedef std::map<DWORD, std::string> StringTable;
std::map<DWORD, StringTable*> g_resourceTables;
DWORD g_nextResourceHandle = 1;

void loadStringTable(const char* fileName, StringTable& out)
{
    std::string path(fileName ? fileName : "");
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) path = path.substr(0, dot);
    path += ".txt";

    // PORT (M8-B10): read each record whole.
    //
    // This was `char line[1024]` with fgets, and fgets stops after 1023
    // bytes WITHOUT consuming the rest of the line. The remainder came back
    // on the next iteration carrying no tab, so the `if (!tab) continue`
    // below threw it away silently. Twenty-three retail strings are longer
    // than that -- every mission briefing in the campaign, plus six
    // Encyclopedia entries -- and each lost its tail with nothing logged.
    //
    // MEASURED on id 55005, the briefing that opened M8-B10: the record is
    // "55005" + tab + 1315 escaped characters. fgets kept 1023 bytes of it,
    // which is the id (5) + tab (1) + exactly 1017 characters of text --
    // and 1017 is precisely where the rendered briefing stopped, mid-word,
    // at "...fire on those tanks fro". The 1024-byte buffer that used to
    // sit in gos_GetResourceString would have cut at 1023 decoded chars, 15
    // characters later than what was on screen, so it was NOT the one that
    // bit here -- it is fixed below as the same mistake one layer up.
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr,
            "[GameOS] resource string table '%s' not found; strings will render as placeholders\n",
            path.c_str());
        return;
    }

    size_t longest = 0;
    DWORD longestId = 0;

    std::string line;
    while (std::getline(in, line))
    {
        const std::string::size_type tabPos = line.find('\t');
        if (tabPos == std::string::npos) continue;
        std::string textStore = line.substr(tabPos + 1);

        size_t n = textStore.size();
        while (n && (textStore[n - 1] == '\n' || textStore[n - 1] == '\r')) --n;
        textStore.resize(n);
        const char* text = textStore.c_str();

        // PORT: The extractor keeps the table one-record-per-line by escaping
        // control characters. Decode them here so retail strings retain their
        // original tabs and line breaks without making the text format ambiguous.
        std::string decoded;
        decoded.reserve(std::strlen(text));
        for (const char* p = text; *p; ++p)
        {
            if (*p != '\\' || !p[1])
            {
                decoded.push_back(*p);
                continue;
            }
            ++p;
            switch (*p)
            {
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            case '\\': decoded.push_back('\\'); break;
            default:
                decoded.push_back('\\');
                decoded.push_back(*p);
                break;
            }
        }
        const DWORD id = (DWORD)std::strtoul(line.c_str(), nullptr, 10);
        if (decoded.size() > longest) { longest = decoded.size(); longestId = id; }
        out[id] = decoded;
    }

    // PORT (M8-B10): the longest string is reported because it is the cheap
    // check that this loader is not truncating. Under the old fgets buffer
    // no string could exceed 1017 characters; the retail table's longest is
    // 2000 (id 45371). A run that reports a maximum at or below 1017 has
    // regressed to the old behaviour.
    std::fprintf(stderr,
                 "[GameOS] loaded %u resource strings from %s (longest %u chars, id %lu)\n",
                 (unsigned)out.size(), path.c_str(),
                 (unsigned)longest, (unsigned long)longestId);
}

} // namespace

DWORD __stdcall gos_OpenResourceDLL(const char* FileName)
{
    StringTable* t = new StringTable();
    loadStringTable(FileName, *t);

    DWORD handle = g_nextResourceHandle++;
    g_resourceTables[handle] = t;
    return handle;
}

void __stdcall gos_CloseResourceDLL(DWORD Handle)
{
    std::map<DWORD, StringTable*>::iterator it = g_resourceTables.find(Handle);
    if (it == g_resourceTables.end()) return;
    delete it->second;
    g_resourceTables.erase(it);
}

char* __stdcall gos_GetResourceString(DWORD Handle, DWORD Id)
{
    // PORT (M8-B10): this was `static char buffer[1024]`, which cut every
    // string at 1023 characters -- the same mistake as the loader's old
    // fgets buffer, one layer up, and equally silent. It is a shared static
    // by the original design (callers copy out of it immediately, which is
    // what cLoadString does), so that contract is kept exactly; only the
    // length cap is gone.
    //
    // NOTE for whoever sees a new STOP from cLoadString: full-length strings
    // can now exceed a caller's buffer where the truncated ones fitted. The
    // longest retail string is 2000 characters (id 45371, an Encyclopedia
    // entry); its consumers pass 4095 (aTextListItem::setText) and 2047
    // (MissionBriefingScreen), so both fit. The one buffer that would not is
    // ablmc2.cpp's tutorialMessages[1023], and no id in the over-long set is
    // tutorial text. cLoadString's STOP names the id and both lengths, which
    // is a better failure than the silent cut this replaces.
    static std::string buffer;

    std::map<DWORD, StringTable*>::const_iterator it = g_resourceTables.find(Handle);
    if (it != g_resourceTables.end())
    {
        StringTable::const_iterator s = it->second->find(Id);
        if (s != it->second->end())
        {
            buffer = s->second;
            return const_cast<char*>(buffer.c_str());
        }
    }

    // MISSING STRING POLICY.
    //
    // An earlier version returned "<str:NNN>" so gaps were visible on screen.
    // That breaks boot: MC2's cLoadString copies into a caller-supplied buffer
    // and STOPs if the string does not fit, and some buffers are tiny --
    // "String too long for buffer. String Id 19900, bufferLen 8, StringLen 11".
    // A placeholder cannot know the caller's buffer size, so any non-empty
    // filler is a latent crash.
    //
    // Empty is the only length that always fits. Each missing id is reported
    // once so the gap is still visible in the log rather than silently swallowed.
    static std::map<DWORD, bool>* reported = new std::map<DWORD, bool>();
    if (!(*reported)[Id])
    {
        (*reported)[Id] = true;
        std::fprintf(stderr, "[GameOS] resource string %lu missing\n", (unsigned long)Id);
    }

    // PORT (M8-5 caller trace): MC2_STRPROBE=1 names WHO asked for a missing id.
    // Retail's table starts at 10000, so id 0 is never a real string; the line
    // above says it was asked for, not by whom. Reports each call (up to 8 per
    // id) with the stack; frame 0 is this function's caller. Off by default.
    static const bool strProbe = []() -> bool {
        const char* env = std::getenv("MC2_STRPROBE");
        return env && env[0] && env[0] != '0';
    }();
    static std::map<DWORD, int>* probed = new std::map<DWORD, int>();
    if (strProbe && (*probed)[Id] < 8)
    {
        const int call = ++(*probed)[Id];
        void* frames[8] = {};
        const int n = MC2_PlatformCaptureBacktrace(frames, 8);
        std::fprintf(stderr, "[STRPROBE] missing id %lu call %d, %d frames\n",
                     (unsigned long)Id, call, n);
        for (int f = 0; f < n; ++f)
        {
            char text[512];
            MC2_PlatformDescribeAddress(frames[f], text, sizeof(text));
            std::fprintf(stderr, "[STRPROBE]   #%d %s\n", f, text);
        }
    }

    buffer.clear();
    return const_cast<char*>(buffer.c_str());
}
