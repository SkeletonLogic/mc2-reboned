//===========================================================================//
// Round-trip test for the ported LZW codec (lzcomp.cpp / lzdecomp.cpp).
//
// Both routines were transcribed by hand from large blocks of x86 assembly, and
// they have to agree bit for bit: LZCompress writes .txm texture caches and save
// games, LZDecomp reads them back. A silent disagreement would not surface until
// a mission failed to load or a save came back corrupt, which is exactly the
// kind of bug that is miserable to find inside the running game.
//
// lzcomp.cpp guards its GameOS includes behind _MBCS, so this target compiles
// both translation units with _MBCS defined and links them free of MCLib.
//
// Note the two files share mutable file-scope state (LZCHashBuf, LZFreeIndex,
// bit-position globals), so cases run strictly sequentially.
//===========================================================================//

#include <cstdio>
#include <cstring>
#include <cstdlib>

typedef unsigned char* MemoryPtr;

extern long LZCompress(MemoryPtr dest, MemoryPtr src, unsigned long srcLen);
extern long LZDecomp  (MemoryPtr dest, MemoryPtr src, unsigned long srcLen);

static int g_failures = 0;
static int g_cases    = 0;

// Compresses, decompresses, and demands the bytes come back identical.
static void roundtrip(const char* name, const unsigned char* data, unsigned long len)
{
    ++g_cases;

    // LZW can expand incompressible input, and the writer always deposits three
    // bytes per code, so the destination needs real slack.
    unsigned long capacity = len * 2 + 1024;

    unsigned char* packed   = (unsigned char*)calloc(1, capacity);
    unsigned char* unpacked = (unsigned char*)calloc(1, len + 1024);

    long packedLen = LZCompress(packed, (MemoryPtr)data, len);

    if (packedLen <= 0 && len > 0)
    {
        std::printf("  FAIL  %-28s compress returned %ld\n", name, packedLen);
        ++g_failures;
        free(packed); free(unpacked);
        return;
    }

    long outLen = LZDecomp(unpacked, packed, (unsigned long)packedLen);

    bool ok = ((unsigned long)outLen == len) &&
              (len == 0 || std::memcmp(unpacked, data, len) == 0);

    if (!ok)
    {
        // Report the first divergence -- far more useful than "mismatch".
        long firstBad = -1;
        unsigned long n = ((unsigned long)outLen < len) ? (unsigned long)outLen : len;
        for (unsigned long i = 0; i < n; ++i)
            if (unpacked[i] != data[i]) { firstBad = (long)i; break; }

        std::printf("  FAIL  %-28s in=%lu packed=%ld out=%ld firstDiff=%ld\n",
                    name, len, packedLen, outLen, firstBad);
        ++g_failures;
    }
    else
    {
        std::printf("  ok    %-28s in=%-8lu packed=%-8ld ratio=%.2f\n",
                    name, len, packedLen, len ? (double)packedLen / (double)len : 0.0);
    }

    free(packed);
    free(unpacked);
}

int main()
{
    std::printf("=== LZW round-trip (lzcomp <-> lzdecomp) ===\n");

    // --- degenerate sizes -------------------------------------------------
    {
        unsigned char one = 'A';
        roundtrip("single byte", &one, 1);
    }
    {
        unsigned char two[2] = { 'A', 'B' };
        roundtrip("two bytes", two, 2);
    }

    // --- highly compressible: long runs exercise chain building ----------
    {
        const unsigned long N = 64 * 1024;
        unsigned char* buf = (unsigned char*)malloc(N);
        std::memset(buf, 'Z', N);
        roundtrip("64K identical bytes", buf, N);
        free(buf);
    }

    // --- repeating pattern: builds deep chains ---------------------------
    {
        const unsigned long N = 32 * 1024;
        unsigned char* buf = (unsigned char*)malloc(N);
        const char* pat = "MechCommander2";
        for (unsigned long i = 0; i < N; ++i) buf[i] = (unsigned char)pat[i % 14];
        roundtrip("repeating pattern", buf, N);
        free(buf);
    }

    // --- incompressible: forces the table to fill, grow to 12 bits, and
    //     ultimately emit Clear codes -- the paths most likely to diverge.
    {
        const unsigned long N = 256 * 1024;
        unsigned char* buf = (unsigned char*)malloc(N);
        unsigned long s = 12345;
        for (unsigned long i = 0; i < N; ++i)
        {
            s = s * 1103515245u + 12345u;      // deterministic, reproducible
            buf[i] = (unsigned char)((s >> 16) & 0xFF);
        }
        roundtrip("256K pseudo-random", buf, N);
        free(buf);
    }

    // --- all byte values, so every literal code is used ------------------
    {
        const unsigned long N = 256 * 64;
        unsigned char* buf = (unsigned char*)malloc(N);
        for (unsigned long i = 0; i < N; ++i) buf[i] = (unsigned char)(i & 0xFF);
        roundtrip("all 256 byte values", buf, N);
        free(buf);
    }

    // --- text, the realistic case for .fit/.abl style data ---------------
    {
        const char* text =
            "Operation Falcon's Nest. Deploy the lance to the drop zone and "
            "secure the resource bay before enemy reinforcements arrive. "
            "Repair bays are available at the staging area. ";
        const unsigned long reps = 400;
        unsigned long tl = (unsigned long)std::strlen(text);
        unsigned long N  = tl * reps;
        unsigned char* buf = (unsigned char*)malloc(N);
        for (unsigned long r = 0; r < reps; ++r) std::memcpy(buf + r * tl, text, tl);
        roundtrip("repeated english text", buf, N);
        free(buf);
    }

    // --- sizes straddling byte/bit boundaries ----------------------------
    for (unsigned long n = 1; n <= 40; ++n)
    {
        unsigned char buf[64];
        for (unsigned long i = 0; i < n; ++i) buf[i] = (unsigned char)(i * 7 + 3);
        char nm[64];
        std::snprintf(nm, sizeof(nm), "ramp len=%lu", n);
        // Only report failures for this sweep to keep the log readable.
        int before = g_failures;
        int cases  = g_cases;
        std::fflush(stdout);
        roundtrip(nm, buf, n);
        if (g_failures == before) { g_cases = cases + 1; }
    }

    std::printf("\n%d case(s), %d failure(s)\n", g_cases, g_failures);
    if (g_failures == 0) std::printf("ALL ROUND-TRIPS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
