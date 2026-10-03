#pragma once
// PORT: CRT spellings that MSVC's precompiled headers supplied implicitly.
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <strings.h>
#include <sys/stat.h>
#include <type_traits>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <map>
#include <set>
#include <string>

// PORT: libc++ already supplies the standard placement-new overload.
#define __PLACEMENT_NEW_INLINE 1

// PORT: Match the 32-bit Windows scalar ABI before legacy headers repeat these
// typedefs. TYPEDEFS1 prevents Vfx.h from selecting its non-Windows WORD type.
typedef unsigned char BYTE;
typedef unsigned char UBYTE;
typedef unsigned short WORD;
typedef unsigned short UWORD;
typedef unsigned long DWORD;
typedef unsigned long ULONG;
typedef long LONG;
typedef float FLOAT;
typedef long FIXED16;
typedef long FIXED30;
#define TYPEDEFS1 1
extern "C" DWORD timeGetTime();

inline std::string mc2_posix_path(const char* path)
{
	std::string result = path ? path : "";
	for (char& value : result) if (value == '\\') value = '/';
	return result;
}

// PORT: RESOLVE PATHS CASE-INSENSITIVELY.
//
// MC2 is a Win32 game and its data references disagree with the shipped
// filenames in case all over the place; NTFS did not care. OPFS/WasmFS is
// case-SENSITIVE, so every such mismatch is a hard file-not-found, and the
// legacy code turns that into STOP() -- which aborts the wasm program and
// leaves the browser tab sitting there looking frozen.
//
// Measured 2026-08-24: saving a campaign as "Bingus" wrote
// data/savegame/Bingus.fit, File::open() lowercases the name (only
// createWithCase does not), the reopen therefore asked for bingus.fit, and
// NEW CAMPAIGN died with
//   [GameOS STOP] couldn't open file data\savegame\bingus.fit
//   program exited (with status: 1)
//
// Walk the path a component at a time and, when a component misses, take the
// directory entry that matches case-insensitively. Successful rewrites are
// remembered; misses are NOT cached, because the caller may be about to
// create the file.
// PORT: REMEMBER WHAT IS NOT THERE.
//
// The legacy code probes for absent files in loops. Measured 2026-08-24: one NEW
// CAMPAIGN click asked whether data/campaign existed 16,865 times. On Win32 that
// was a cheap in-kernel lookup; on OPFS every one of them is a round-trip to the
// storage worker through Asyncify, which is 30 s of a frozen browser tab.
//
// So remember misses and answer them from memory. Any successful create clears
// the table, so a file written this session is still found afterwards.
#include "mc2_path_cache.h"

// Only ever called AFTER the exact path has already failed. It must never be
// put in front of a successful open: doing that added a stat to every file the
// game touches and cost 20,711 OPFS round-trips on a single NEW CAMPAIGN click.
inline std::string mc2_resolve_path(std::string posix)
{
	struct stat probe;
	if (posix.empty())
		return posix;
	std::map<std::string, std::string>& resolved = mc2_resolved_paths();
	std::map<std::string, std::string>::iterator cached = resolved.find(posix);
	if (cached != resolved.end())
		return cached->second;
	if (mc2_under_missing_dir(posix))
		return posix;
	const bool absolute = posix[0] == '/';
	std::string built = absolute ? std::string("/") : std::string();
	std::string rest = absolute ? posix.substr(1) : posix;
	std::size_t start = 0;
	bool ok = true;
	while (start <= rest.size())
	{
		const std::size_t slash = rest.find('/', start);
		const std::string part = rest.substr(start,
			slash == std::string::npos ? std::string::npos : slash - start);
		if (!part.empty())
		{
			std::string candidate = built;
			if (!candidate.empty() && candidate[candidate.size() - 1] != '/') candidate += '/';
			candidate += part;
			if (::stat(candidate.c_str(), &probe) == 0)
			{
				built = candidate;
			}
			else
			{
				// PORT: scan each directory ONCE per session. Measured 2026-09-24:
				// ?mission=mc2_10 sat for minutes after [shadow-dist] because each
				// of the ~475 terrain overlays (textures/64overlays/..., which live
				// in textures.fst, not on disk) re-listed data/textures -- about
				// 4,500 entries -- through OPFS, ~3 lookups a second.
				const std::string parent = built.empty() ? std::string(".") : built;
				std::map<std::string, std::map<std::string, std::string>>& listings = mc2_dir_listings();
				std::map<std::string, std::map<std::string, std::string>>::iterator listing = listings.find(parent);
				if (listing == listings.end())
				{
					std::map<std::string, std::string> names;
					if (DIR* dir = ::opendir(parent.c_str()))
					{
						for (dirent* entry = ::readdir(dir); entry; entry = ::readdir(dir))
						{
							std::string lower = entry->d_name;
							for (char& c : lower) c = (char)std::tolower((unsigned char)c);
							names.insert(std::make_pair(lower, std::string(entry->d_name)));
						}
						::closedir(dir);
					}
					listing = listings.insert(std::make_pair(parent, names)).first;
				}
				std::string lowerPart = part;
				for (char& c : lowerPart) c = (char)std::tolower((unsigned char)c);
				std::map<std::string, std::string>::const_iterator match = listing->second.find(lowerPart);
				if (match == listing->second.end())
				{
					// A missing directory answers for everything beneath it.
					if (slash != std::string::npos) mc2_missing_dirs().insert(candidate);
					ok = false;
					break;
				}
				if (!built.empty() && built[built.size() - 1] != '/') built += '/';
				built += match->second;
			}
		}
		if (slash == std::string::npos) break;
		start = slash + 1;
	}
	// Cache the MISS as well. The game probes for absent files in loops -- a single
	// NEW CAMPAIGN click asked whether data/campaign existed 16,865 times -- and an
	// uncached miss re-walks every component, which is 30 s of OPFS round-trips.
	// mc2_path_cache_invalidate() drops every table whenever something is created,
	// so a save can still be reopened in the same session.
	resolved[posix] = ok ? built : posix;
	return ok ? built : posix;
}
// Any file creation can turn a cached miss into a hit, so throw the tables away.
// Creation is rare; this costs nothing measurable.
inline void mc2_forget_resolved_paths()
{
	mc2_path_cache_invalidate();
}
inline int mc2_stat(const char* path, struct stat* info)
{
	const std::string exact = mc2_posix_path(path);
	if (mc2_missing_paths().count(exact) || mc2_under_missing_dir(exact)) return -1;
	if (::stat(exact.c_str(), info) == 0) return 0;
	const std::string alt = mc2_resolve_path(exact);
	if (alt != exact && ::stat(alt.c_str(), info) == 0) return 0;
	mc2_missing_paths().insert(exact);
	return -1;
}
// PORT: READ SMALL FILES IN ONE STORAGE TRIP.
//
// MCLib's File reads in tiny pieces -- readLine/readByte/readLong each call
// _read -- and on OPFS every _read is an Asyncify round-trip to the storage
// worker. Measured 2026-09-24 on the owner's route: the NEW CAMPAIGN save
// dialog opens every .fit in data/savegame to show its campaign name, and 28
// saves (4 MB) took over four minutes -- 339,000 storage round-trips, about
// 1,300 a second, with the game loop parked the whole time.
//
// So a file opened READ-ONLY and no larger than kMc2ReadCacheLimit is read
// whole at open and served from memory. The descriptor stays open, so fstat
// and _filelength still answer from storage. Larger files (sound paks, movies)
// keep streaming. Writable opens are never cached.
//
// The same holds for WRITES, measured the same day: an in-mission save
// (testgame.ims, ~1.9 MB) went out a few bytes per _write, ~210 storage writes
// a second plus a size query after each, freezing the game for minutes. So a
// file CREATED through _creat is assembled in memory and written to storage in
// one call when it is closed. Until then storage holds an empty file.
struct Mc2ReadCacheEntry { std::string data; long pos; bool writing = false; };
inline std::map<int, Mc2ReadCacheEntry>& mc2_read_cache()
{
	static std::map<int, Mc2ReadCacheEntry> table;
	return table;
}
const long kMc2ReadCacheLimit = 8L * 1024L * 1024L;
// Diagnostic switches, read once: MC2_BROWSER_READ_CACHE=0 / MC2_BROWSER_WRITE_CACHE=0
// turn either side off so a fault can be pinned on one of them or neither.
inline bool mc2_cache_switch(const char* name)
{
	const char* v = std::getenv(name);
	return !(v && std::strcmp(v, "0") == 0);
}
inline bool mc2_read_cache_on()  { static const bool on = mc2_cache_switch("MC2_BROWSER_READ_CACHE");  return on; }
inline bool mc2_write_cache_on() { static const bool on = mc2_cache_switch("MC2_BROWSER_WRITE_CACHE"); return on; }
inline int mc2_read_cache_attach(int fd, int flags)
{
	if (!mc2_read_cache_on()) return fd;
	if (fd < 0 || (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0) return fd;
	struct stat info;
	if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
	    info.st_size > kMc2ReadCacheLimit) return fd;
	Mc2ReadCacheEntry entry;
	entry.data.resize((std::size_t)info.st_size);
	std::size_t got = 0;
	while (got < entry.data.size())
	{
		const ssize_t n = ::read(fd, &entry.data[got], entry.data.size() - got);
		if (n <= 0) break;
		got += (std::size_t)n;
	}
	if (got != entry.data.size()) { ::lseek(fd, 0, SEEK_SET); return fd; }   // fall back to streaming
	entry.pos = 0;
	mc2_read_cache()[fd] = entry;
	return fd;
}
inline ssize_t mc2_read(int fd, void* buffer, std::size_t count)
{
	std::map<int, Mc2ReadCacheEntry>::iterator it = mc2_read_cache().find(fd);
	if (it == mc2_read_cache().end()) return ::read(fd, buffer, count);
	Mc2ReadCacheEntry& e = it->second;
	const long size = (long)e.data.size();
	if (e.pos >= size) return 0;
	const std::size_t n = std::min<std::size_t>(count, (std::size_t)(size - e.pos));
	std::memcpy(buffer, e.data.data() + e.pos, n);
	e.pos += (long)n;
	return (ssize_t)n;
}
inline off_t mc2_lseek(int fd, off_t offset, int whence)
{
	std::map<int, Mc2ReadCacheEntry>::iterator it = mc2_read_cache().find(fd);
	if (it == mc2_read_cache().end()) return ::lseek(fd, offset, whence);
	Mc2ReadCacheEntry& e = it->second;
	long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? e.pos : (long)e.data.size();
	const long target = base + (long)offset;
	if (target < 0) { errno = EINVAL; return (off_t)-1; }
	e.pos = target;   // past-EOF seeks are legal; reads there return 0
	return (off_t)target;
}
inline int mc2_write_cache_attach(int fd)
{
	if (fd < 0 || !mc2_write_cache_on()) return fd;
	Mc2ReadCacheEntry entry;
	entry.pos = 0;
	entry.writing = true;
	mc2_read_cache()[fd] = entry;
	return fd;
}
inline ssize_t mc2_write(int fd, const void* buffer, std::size_t count)
{
	std::map<int, Mc2ReadCacheEntry>::iterator it = mc2_read_cache().find(fd);
	if (it == mc2_read_cache().end() || !it->second.writing) return ::write(fd, buffer, count);
	Mc2ReadCacheEntry& e = it->second;
	const std::size_t end = (std::size_t)e.pos + count;
	if (end > e.data.size()) e.data.resize(end);
	std::memcpy(&e.data[(std::size_t)e.pos], buffer, count);
	e.pos = (long)end;
	return (ssize_t)count;
}
inline long mc2_filelength(int fd)
{
	std::map<int, Mc2ReadCacheEntry>::iterator it = mc2_read_cache().find(fd);
	if (it != mc2_read_cache().end()) return (long)it->second.data.size();
	struct stat info {};
	return ::fstat(fd, &info) == 0 ? static_cast<long>(info.st_size) : -1L;
}
inline int mc2_close(int fd)
{
	std::map<int, Mc2ReadCacheEntry>::iterator it = mc2_read_cache().find(fd);
	if (it != mc2_read_cache().end())
	{
		if (it->second.writing && !it->second.data.empty())
		{
			const std::string& d = it->second.data;
			::lseek(fd, 0, SEEK_SET);
			std::size_t done = 0;
			while (done < d.size())
			{
				const ssize_t n = ::write(fd, d.data() + done, d.size() - done);
				if (n <= 0) break;
				done += (std::size_t)n;
			}
			if (done != d.size())
				std::printf("[vfs] WRITE-BACK SHORT fd=%d wrote %zu of %zu bytes\n", fd, done, d.size());
		}
		mc2_read_cache().erase(it);
	}
	return ::close(fd);
}
inline int mc2_open(const char* path, int flags)
{
	const std::string exact = mc2_posix_path(path);
	if (!(flags & O_CREAT) && (mc2_missing_paths().count(exact) || mc2_under_missing_dir(exact))) return -1;
	const int fd = ::open(exact.c_str(), flags);
	if (flags & O_CREAT) { if (fd >= 0) mc2_path_cache_note_file(exact); return fd; }   // M8-B77
	if (fd >= 0) return mc2_read_cache_attach(fd, flags);
	const std::string alt = mc2_resolve_path(exact);
	if (alt != exact)
	{
		const int viaCase = ::open(alt.c_str(), flags);
		if (viaCase >= 0) return mc2_read_cache_attach(viaCase, flags);
	}
	mc2_missing_paths().insert(exact);
	return fd;
}
inline int mc2_open(const char* path, int flags, int mode)
{
	if (flags & O_CREAT) mode |= S_IRUSR | S_IWUSR;   // see mc2_creat
	const std::string exact = mc2_posix_path(path);
	if (!(flags & O_CREAT) && (mc2_missing_paths().count(exact) || mc2_under_missing_dir(exact))) return -1;
	const int fd = ::open(exact.c_str(), flags, mode);
	if (flags & O_CREAT) { if (fd >= 0) mc2_path_cache_note_file(exact); return fd; }   // M8-B77
	if (fd >= 0) return mc2_read_cache_attach(fd, flags);
	const std::string alt = mc2_resolve_path(exact);
	if (alt != exact)
	{
		const int viaCase = ::open(alt.c_str(), flags, mode);
		if (viaCase >= 0) return mc2_read_cache_attach(viaCase, flags);
	}
	mc2_missing_paths().insert(exact);
	return fd;
}
inline int mc2_creat(const char* path, int mode)
{
	// PORT: Windows _creat(name, _S_IWRITE) means "not read-only"; the file is
	// still readable. POSIX takes the same value (S_IWUSR, 0200) as WRITE-ONLY
	// permission, and WasmFS enforces it: measured 2026-09-24, every in-mission
	// save wrote its temp .fti/.abl as mode 0200, the read-back open failed with
	// ENOENT while stat found the file, and the save stored an EMPTY settings
	// packet -- so loading it stopped with "Mission Data missing". Keep Windows
	// semantics: a created file is always readable and writable by its owner.
	mode |= S_IRUSR | S_IWUSR;
	// Creating: prefer an existing differently-cased file over making a twin.
	const std::string exact = mc2_posix_path(path);
	struct stat probe;
	if (::stat(exact.c_str(), &probe) != 0)
	{
		const std::string alt = mc2_resolve_path(exact);
		if (alt != exact) return mc2_write_cache_attach(::creat(alt.c_str(), mode));
	}
	const int created = ::creat(exact.c_str(), mode);
	if (created >= 0) mc2_path_cache_note_file(exact);   // M8-B77
	return mc2_write_cache_attach(created);
}
inline int mc2_access(const char* path, int mode)
{
	const std::string exact = mc2_posix_path(path);
	if (mc2_missing_paths().count(exact) || mc2_under_missing_dir(exact)) return -1;
	if (::access(exact.c_str(), mode) == 0) return 0;
	const std::string alt = mc2_resolve_path(exact);
	if (alt != exact && ::access(alt.c_str(), mode) == 0) return 0;
	mc2_missing_paths().insert(exact);
	return -1;
}
#define _stat stat
#define _open mc2_open
#define _creat mc2_creat
#define _access mc2_access

#define stricmp strcasecmp
#define strcmpi strcasecmp
#define strnicmp strncasecmp
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _int64 long long

inline char* mc2_strlwr(char* text)
{
	if (text) {
		for (char* cursor = text; *cursor; ++cursor)
			*cursor = static_cast<char>(std::tolower(static_cast<unsigned char>(*cursor)));
	}
	return text;
}
#define _strlwr mc2_strlwr

inline char* mc2_itoa(int value, char* buffer, int radix)
{
	if (!buffer || radix < 2 || radix > 36) return buffer;
	char scratch[35];
	unsigned int magnitude = value < 0 ? 0u - static_cast<unsigned int>(value) :
		static_cast<unsigned int>(value);
	int count = 0;
	do {
		const unsigned int digit = magnitude % static_cast<unsigned int>(radix);
		scratch[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
		magnitude /= static_cast<unsigned int>(radix);
	} while (magnitude);
	char* output = buffer;
	if (value < 0 && radix == 10) *output++ = '-';
	while (count) *output++ = scratch[--count];
	*output = '\0';
	return buffer;
}
#define _itoa mc2_itoa

inline void mc2_splitpath(const char* path, char* drive, char* directory,
	char* name, char* extension)
{
	if (drive) drive[0] = '\0';
	const char* source = path ? path : "";
	const char* slash = std::strrchr(source, '/');
	const char* backslash = std::strrchr(source, '\\');
	if (!slash || (backslash && backslash > slash)) slash = backslash;
	const char* base = slash ? slash + 1 : source;
	const char* dot = std::strrchr(base, '.');
	if (directory) {
		const std::size_t count = slash ? static_cast<std::size_t>(slash - source + 1) : 0;
		std::memcpy(directory, source, count);
		directory[count] = '\0';
	}
	if (name) {
		const std::size_t count = dot ? static_cast<std::size_t>(dot - base) : std::strlen(base);
		std::memcpy(name, base, count);
		name[count] = '\0';
	}
	if (extension) std::strcpy(extension, dot ? dot : "");
}
#define _splitpath mc2_splitpath
#define _chmod chmod
#define _isnan std::isnan
template<typename T, typename U>
constexpr typename std::common_type<T, U>::type __min(const T& left, const U& right)
{
	using Result = typename std::common_type<T, U>::type;
	return right < left ? static_cast<Result>(right) : static_cast<Result>(left);
}
template<typename T, typename U>
constexpr typename std::common_type<T, U>::type __max(const T& left, const U& right)
{
	using Result = typename std::common_type<T, U>::type;
	return left < right ? static_cast<Result>(right) : static_cast<Result>(left);
}
