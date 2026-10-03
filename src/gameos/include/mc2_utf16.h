#ifndef MC2_UTF16_H
#define MC2_UTF16_H

// PORT: The legacy string layer stores Windows UTF-16 code units even when
// wchar_t is distinct or wider. Keep that storage contract explicit without
// leaking windows.h into every EString consumer.

#include <cstddef>
#include <cstdint>
#include <cstdarg>

using MC2Utf16 = std::uint16_t;
static_assert(sizeof(MC2Utf16) == 2, "MC2 UTF-16 code units must be 16 bits");

namespace mc2_utf16
{

std::size_t length(const MC2Utf16* text);
MC2Utf16* reverse(MC2Utf16* text);
MC2Utf16* find(const MC2Utf16* text, const MC2Utf16* needle);
int to_int(const MC2Utf16* text);
int from_mb(const char* source, MC2Utf16* destination, int destinationCount);
int to_mb(const MC2Utf16* source, char* destination, int destinationCount);
MC2Utf16* upper(MC2Utf16* text);
MC2Utf16* lower(MC2Utf16* text);
int compare_i(const MC2Utf16* left, const MC2Utf16* right);
unsigned short classify(MC2Utf16 value);
int format(MC2Utf16* destination, const MC2Utf16* formatText, va_list arguments);

} // namespace mc2_utf16

#endif
