#ifndef MC2_WINDOWS_COMPAT_H
#define MC2_WINDOWS_COMPAT_H

// PORT: Minimal declarations needed by legacy public headers on non-Windows
// hosts. This is a type/constant compatibility surface, not a Win32 emulation.

#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <strings.h>
#include <ctime>
#include <cstdlib>
#include <cerrno>
#if defined(__EMSCRIPTEN__)
#include <dirent.h>
#endif
#include <sys/stat.h>
#if defined(__EMSCRIPTEN__)
#include <string>
#include <vector>
#endif

using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using DWORD = unsigned long;
using UINT = unsigned int;
using ULONG = unsigned long;
using LONG = long;
using SHORT = short;
using BOOL = int;
using HRESULT = std::int32_t;
using WCHAR = std::uint16_t;
using HANDLE = void*;
using HKEY = HANDLE;
using HWND = HANDLE;
using HINSTANCE = HANDLE;
using HMODULE = HANDLE;
using LPVOID = void*;
using LPCVOID = const void*;
using LPSTR = char*;
using LPCSTR = const char*;
using LPWSTR = WCHAR*;
using LPCWSTR = const WCHAR*;
using PBYTE = BYTE*;
using LPCTSTR = const char*;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;

struct POINT
{
	LONG x;
	LONG y;
};

struct tagRECT
{
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
};
using RECT = tagRECT;

struct _WIN32_FIND_DATA
{
	DWORD dwFileAttributes = 0;
	char cFileName[260] = {};
};
using WIN32_FIND_DATA = _WIN32_FIND_DATA;

struct MEMORYSTATUS
{
	DWORD dwLength = 0;
	DWORD dwMemoryLoad = 0;
	DWORD dwTotalPhys = 0;
	DWORD dwAvailPhys = 0;
	DWORD dwTotalPageFile = 0;
	DWORD dwAvailPageFile = 0;
	DWORD dwTotalVirtual = 0;
	DWORD dwAvailVirtual = 0;
};

struct DEVMODE
{
	WORD dmSize = 0;
	WORD dmSpecVersion = 0;
	DWORD dmPelsWidth = 1024;
	DWORD dmPelsHeight = 768;
	DWORD dmBitsPerPel = 32;
};

struct SYSTEMTIME
{
	WORD wYear, wMonth, wDayOfWeek, wDay;
	WORD wHour, wMinute, wSecond, wMilliseconds;
};

struct TIME_ZONE_INFORMATION
{
	LONG Bias = 0;
	WCHAR StandardName[32] = {};
	SYSTEMTIME StandardDate = {};
	LONG StandardBias = 0;
	WCHAR DaylightName[32] = {};
	SYSTEMTIME DaylightDate = {};
	LONG DaylightBias = 0;
};

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif

#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005L)
#define SUCCEEDED(value) ((HRESULT)(value) >= 0)
#define FAILED(value) ((HRESULT)(value) < 0)
#define MAX_PATH 260
#define LOCALE_SYSTEM_DEFAULT 0x0800
#define CT_CTYPE1 0x00000001
#define C1_DIGIT 0x0004
#define C1_XDIGIT 0x0080
#define MB_OKCANCEL 0x00000001L
#define MB_OK 0x00000000L
#define MB_ICONWARNING 0x00000030L
#define IDOK 1
#define IDCANCEL 2
#define TIME_ZONE_ID_UNKNOWN 0
#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_DAYLIGHT 2
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define PAGE_READWRITE 0x00000004
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define INVALID_HANDLE_VALUE ((HANDLE)(std::intptr_t)-1)
#define DM_SPECVERSION 0x0401
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#define ERROR_SUCCESS 0L
#define HKEY_CURRENT_USER ((HKEY)(std::intptr_t)1)
#define GENERIC_READ 0x80000000UL
#define OPEN_EXISTING 3
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define SM_SWAPBUTTON 23

#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef _stdcall
#define _stdcall
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef _declspec
#define _declspec(value) __declspec(value)
#endif

inline int lstrlen(const char* text) { return text ? static_cast<int>(std::strlen(text)) : 0; }
inline int lstrlenA(const char* text) { return lstrlen(text); }
inline int lstrlen(const WCHAR* text)
{
	if (!text) return 0;
	const WCHAR* end = text;
	while (*end) ++end;
	return static_cast<int>(end - text);
}
inline int wcslen(const WCHAR* text) { return lstrlen(text); }
inline int lstrcmpi(const char* left, const char* right) { return strcasecmp(left, right); }
inline char* CharLower(char* text) { return mc2_strlwr(text); }
inline char* CharUpper(char* text)
{
	if (text) {
		for (char* cursor = text; *cursor; ++cursor)
			*cursor = static_cast<char>(std::toupper(static_cast<unsigned char>(*cursor)));
	}
	return text;
}

inline int MessageBox(HWND, const char*, const char*, UINT) { return IDCANCEL; }
inline BOOL GetStringTypeEx(DWORD, DWORD, const char* text, int count, WORD* types)
{
	if (!text || !types || count <= 0) return FALSE;
	for (int index = 0; index < count; ++index) {
		const unsigned char value = static_cast<unsigned char>(text[index]);
		types[index] = std::isdigit(value) ? C1_DIGIT :
			(std::isxdigit(value) ? C1_XDIGIT : 0);
	}
	return TRUE;
}

inline DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION* information)
{
	if (information) *information = TIME_ZONE_INFORMATION{};
	return TIME_ZONE_ID_UNKNOWN;
}

inline void GetSystemTime(SYSTEMTIME* output)
{
	if (!output) return;
	const std::time_t now = std::time(nullptr);
	const std::tm* value = std::gmtime(&now);
	if (!value) { *output = SYSTEMTIME{}; return; }
	output->wYear = static_cast<WORD>(value->tm_year + 1900);
	output->wMonth = static_cast<WORD>(value->tm_mon + 1);
	output->wDayOfWeek = static_cast<WORD>(value->tm_wday);
	output->wDay = static_cast<WORD>(value->tm_mday);
	output->wHour = static_cast<WORD>(value->tm_hour);
	output->wMinute = static_cast<WORD>(value->tm_min);
	output->wSecond = static_cast<WORD>(value->tm_sec);
	output->wMilliseconds = 0;
}

inline DWORD GetLastError() { return static_cast<DWORD>(errno); }
inline LPVOID VirtualAlloc(LPVOID address, std::size_t size, DWORD type, DWORD)
{
	if (address && (type & MEM_COMMIT)) return address;
	return std::calloc(1, size);
}
inline BOOL VirtualFree(LPVOID address, std::size_t, DWORD type)
{
	if (type & MEM_RELEASE) std::free(address);
	return TRUE;
}
inline void FillMemory(LPVOID address, std::size_t size, BYTE value) { std::memset(address, value, size); }
inline void OutputDebugString(const char* text) { if (text) std::fputs(text, stderr); }
inline BOOL IsBadReadPtr(const void* address, std::size_t) { return address ? FALSE : TRUE; }
inline BOOL GetWindowRect(HWND, RECT* rect)
{
	if (rect) *rect = RECT{0, 0, 0, 0};
	return TRUE;
}
inline BOOL GetCursorPos(POINT* point)
{
	if (point) *point = POINT{0, 0};
	return TRUE;
}
inline BOOL CreateDirectory(const char* path, void*)
{
	const bool made = ::mkdir(mc2_posix_path(path).c_str(), 0777) == 0;
#if defined(__EMSCRIPTEN__)
	// A new directory can turn a remembered miss into a hit. Without this the
	// save directory stays "missing" for the rest of the session.
	if (made) mc2_forget_resolved_paths();
#endif
	return made || errno == EEXIST;
}
inline BOOL DeleteFile(const char* path)
{
	const bool removed = std::remove(mc2_posix_path(path).c_str()) == 0;
#if defined(__EMSCRIPTEN__)
	if (removed) mc2_path_cache_note_removed(mc2_posix_path(path));   // M8-B77
#endif
	return removed;
}
inline BOOL SetFileAttributes(const char*, DWORD) { return TRUE; }

#if defined(__EMSCRIPTEN__)
struct MC2FindFileState
{
	std::string directory;
	std::vector<std::string> matches;
	std::size_t index = 0;
};

inline std::string mc2_lower_ascii(std::string value)
{
	for (char& ch : value)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return value;
}

inline bool mc2_find_pattern_matches(const std::string& name, const std::string& pattern)
{
	const std::string lowerName = mc2_lower_ascii(name);
	const std::string lowerPattern = mc2_lower_ascii(pattern);
	const std::size_t star = lowerPattern.find('*');
	if (star == std::string::npos)
		return lowerName == lowerPattern;
	const std::string prefix = lowerPattern.substr(0, star);
	const std::string suffix = lowerPattern.substr(star + 1);
	return lowerName.size() >= prefix.size() + suffix.size()
		&& lowerName.compare(0, prefix.size(), prefix) == 0
		&& lowerName.compare(lowerName.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline BOOL mc2_fill_find_data(const std::string& directory, const std::string& name, WIN32_FIND_DATA* data)
{
	if (!data) return FALSE;
	std::memset(data, 0, sizeof(*data));
	std::snprintf(data->cFileName, sizeof(data->cFileName), "%s", name.c_str());
	std::string fullPath = directory;
	if (!fullPath.empty() && fullPath.back() != '/') fullPath += '/';
	fullPath += name;
	struct stat st = {};
	if (::stat(fullPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
		data->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
	else
		data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	return TRUE;
}

inline HANDLE FindFirstFile(const char* pattern, WIN32_FIND_DATA* data)
{
	const std::string normalized = mc2_posix_path(pattern);
	const std::size_t slash = normalized.find_last_of('/');
	const std::string directory = slash == std::string::npos ? "." : normalized.substr(0, slash);
	const std::string filePattern = slash == std::string::npos ? normalized : normalized.substr(slash + 1);
	DIR* dir = ::opendir(directory.c_str());
	if (!dir) return INVALID_HANDLE_VALUE;

	MC2FindFileState* state = new MC2FindFileState();
	state->directory = directory;
	for (dirent* entry = ::readdir(dir); entry; entry = ::readdir(dir))
	{
		const std::string name = entry->d_name;
		if (name == "." || name == "..") continue;
		if (mc2_find_pattern_matches(name, filePattern))
			state->matches.push_back(name);
	}
	::closedir(dir);

	if (state->matches.empty())
	{
		delete state;
		return INVALID_HANDLE_VALUE;
	}

	mc2_fill_find_data(directory, state->matches[0], data);
	state->index = 1;
	return static_cast<HANDLE>(state);
}

inline BOOL FindNextFile(HANDLE handle, WIN32_FIND_DATA* data)
{
	if (handle == INVALID_HANDLE_VALUE || !handle) return FALSE;
	MC2FindFileState* state = static_cast<MC2FindFileState*>(handle);
	if (state->index >= state->matches.size()) return FALSE;
	mc2_fill_find_data(state->directory, state->matches[state->index], data);
	++state->index;
	return TRUE;
}

inline BOOL FindClose(HANDLE handle)
{
	if (handle != INVALID_HANDLE_VALUE && handle)
		delete static_cast<MC2FindFileState*>(handle);
	return TRUE;
}
#else
inline HANDLE FindFirstFile(const char*, WIN32_FIND_DATA*) { return INVALID_HANDLE_VALUE; }
inline BOOL FindNextFile(HANDLE, WIN32_FIND_DATA*) { return FALSE; }
inline BOOL FindClose(HANDLE) { return TRUE; }
#endif
inline HANDLE CreateFile(const char* path, DWORD, DWORD, void*, DWORD, DWORD, HANDLE)
{
	return static_cast<HANDLE>(std::fopen(mc2_posix_path(path).c_str(), "rb"));
}
inline BOOL CloseHandle(HANDLE) { return TRUE; }
inline LONG RegOpenKey(HKEY, const char*, HKEY*) { return 2L; }
inline LONG RegQueryValueEx(HKEY, const char*, DWORD*, DWORD*, PBYTE, DWORD*) { return 2L; }
inline LONG RegCloseKey(HKEY) { return ERROR_SUCCESS; }
inline BOOL IsDBCSLeadByte(BYTE) { return FALSE; }
inline UINT GetACP() { return 1252; }
inline void GlobalMemoryStatus(MEMORYSTATUS* status)
{
	if (!status) return;
	status->dwTotalPageFile = 0xffffffffUL;
	status->dwAvailPageFile = 0xffffffffUL;
}
inline BOOL EnumDisplaySettings(const char*, DWORD, DEVMODE* mode)
{
	if (mode) { mode->dmPelsWidth = 1024; mode->dmPelsHeight = 768; mode->dmBitsPerPel = 32; }
	return TRUE;
}
inline BOOL CopyFile(const char* source, const char* destination, BOOL failIfExists)
{
	if (failIfExists) {
		FILE* existing = std::fopen(mc2_posix_path(destination).c_str(), "rb");
		if (existing) { std::fclose(existing); return FALSE; }
	}
	FILE* input = std::fopen(mc2_posix_path(source).c_str(), "rb");
	if (!input) return FALSE;
	FILE* output = std::fopen(mc2_posix_path(destination).c_str(), "wb");
	if (!output) { std::fclose(input); return FALSE; }
#if defined(__EMSCRIPTEN__)
	mc2_path_cache_note_file(mc2_posix_path(destination));   // M8-B77
#endif
	char buffer[16384];
	std::size_t count = 0;
	while ((count = std::fread(buffer, 1, sizeof(buffer), input)) != 0)
		if (std::fwrite(buffer, 1, count, output) != count) break;
	const BOOL success = std::ferror(input) == 0 && std::ferror(output) == 0;
	std::fclose(output);
	std::fclose(input);
	return success;
}
inline int GetSystemMetrics(int) { return 0; }
inline SHORT GetAsyncKeyState(int) { return 0; }

// PORT: Functions preserve qualified std::min/std::max calls; Win32-style
// macros would corrupt them during preprocessing.
template<typename T>
constexpr const T& min(const T& left, const T& right)
{
	return right < left ? right : left;
}

template<typename T>
constexpr const T& max(const T& left, const T& right)
{
	return left < right ? right : left;
}

#endif
