// PORT: Host adapters for the legacy fixed-width UTF-16 string layer.

#include "mc2_utf16.h"

#include <cwchar>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mc2_utf16
{

std::size_t length(const MC2Utf16* text)
{
	const MC2Utf16* end = text;
	while (*end)
		++end;
	return static_cast<std::size_t>(end - text);
}

MC2Utf16* reverse(MC2Utf16* text)
{
	const std::size_t count = length(text);
	for (std::size_t left = 0, right = count ? count - 1 : 0; left < right; ++left, --right)
	{
		const MC2Utf16 value = text[left];
		text[left] = text[right];
		text[right] = value;
	}
	return text;
}

MC2Utf16* find(const MC2Utf16* text, const MC2Utf16* needle)
{
	if (!*needle)
		return const_cast<MC2Utf16*>(text);
	for (const MC2Utf16* start = text; *start; ++start)
	{
		const MC2Utf16* left = start;
		const MC2Utf16* right = needle;
		while (*left && *right && *left == *right)
		{
			++left;
			++right;
		}
		if (!*right)
			return const_cast<MC2Utf16*>(start);
	}
	return nullptr;
}

int to_int(const MC2Utf16* text)
{
	while (*text == ' ' || (*text >= '\t' && *text <= '\r'))
		++text;
	int sign = 1;
	if (*text == '-' || *text == '+')
	{
		if (*text++ == '-')
			sign = -1;
	}
	int value = 0;
	while (*text >= '0' && *text <= '9')
		value = value * 10 + (*text++ - '0');
	return sign * value;
}

int from_mb(const char* source, MC2Utf16* destination, int destinationCount)
{
#if defined(_WIN32)
	static_assert(sizeof(wchar_t) == sizeof(MC2Utf16), "Windows wchar_t must be UTF-16");
	return MultiByteToWideChar(CP_ACP, 0, source, -1,
		reinterpret_cast<wchar_t*>(destination), destinationCount);
#else
	// PORT: The web fallback intentionally preserves single-byte values. Full
	// locale/code-page conversion is deferred until localized assets need it.
	int written = 0;
	while (written < destinationCount && source[written])
	{
		destination[written] = static_cast<unsigned char>(source[written]);
		++written;
	}
	if (written < destinationCount)
		destination[written++] = 0;
	return written;
#endif
}

int to_mb(const MC2Utf16* source, char* destination, int destinationCount)
{
#if defined(_WIN32)
	static_assert(sizeof(wchar_t) == sizeof(MC2Utf16), "Windows wchar_t must be UTF-16");
	return WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<const wchar_t*>(source), -1,
		destination, destinationCount, nullptr, nullptr);
#else
	// PORT: Mirror the reduced single-byte fallback used by from_mb().
	int written = 0;
	while (written < destinationCount && source[written])
	{
		destination[written] = source[written] <= 0xff ? static_cast<char>(source[written]) : '?';
		++written;
	}
	if (written < destinationCount)
		destination[written++] = 0;
	return written;
#endif
}

MC2Utf16* upper(MC2Utf16* text)
{
#if defined(_WIN32)
	CharUpperW(reinterpret_cast<wchar_t*>(text));
#else
	// PORT: Non-Windows case folding is deliberately ASCII-only for now.
	for (MC2Utf16* cursor = text; *cursor; ++cursor)
		if (*cursor >= 'a' && *cursor <= 'z')
			*cursor -= 'a' - 'A';
#endif
	return text;
}

MC2Utf16* lower(MC2Utf16* text)
{
#if defined(_WIN32)
	CharLowerW(reinterpret_cast<wchar_t*>(text));
#else
	// PORT: Non-Windows case folding is deliberately ASCII-only for now.
	for (MC2Utf16* cursor = text; *cursor; ++cursor)
		if (*cursor >= 'A' && *cursor <= 'Z')
			*cursor += 'a' - 'A';
#endif
	return text;
}

int compare_i(const MC2Utf16* left, const MC2Utf16* right)
{
#if defined(_WIN32)
	return lstrcmpiW(reinterpret_cast<const wchar_t*>(left),
		reinterpret_cast<const wchar_t*>(right));
#else
	// PORT: Non-Windows comparison follows the ASCII-only folding above.
	while (*left && *right)
	{
		MC2Utf16 l = *left++;
		MC2Utf16 r = *right++;
		if (l >= 'A' && l <= 'Z') l += 'a' - 'A';
		if (r >= 'A' && r <= 'Z') r += 'a' - 'A';
		if (l != r) return l < r ? -1 : 1;
	}
	return *left ? 1 : (*right ? -1 : 0);
#endif
}

unsigned short classify(MC2Utf16 value)
{
#if defined(_WIN32)
	unsigned short type = 0;
	GetStringTypeExW(LOCALE_SYSTEM_DEFAULT, CT_CTYPE1,
		reinterpret_cast<const wchar_t*>(&value), 1, &type);
	return type;
#else
	// PORT: Only the digit flags consumed by the legacy formatter are needed.
	if (value >= '0' && value <= '9') return 0x0004; // C1_DIGIT
	if ((value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F')) return 0x0080; // C1_XDIGIT
	return 0;
#endif
}

int format(MC2Utf16* destination, const MC2Utf16* formatText, va_list arguments)
{
#if defined(_WIN32)
	return vswprintf(reinterpret_cast<wchar_t*>(destination),
		reinterpret_cast<const wchar_t*>(formatText), arguments);
#else
	// PORT: No current game call site formats EWCharString on the web path.
	// Fail closed until a UTF-16-aware formatter is required.
	(void)formatText;
	(void)arguments;
	destination[0] = 0;
	return -1;
#endif
}

} // namespace mc2_utf16
