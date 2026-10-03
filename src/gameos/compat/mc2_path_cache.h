#pragma once
// PORT: the browser's path-lookup memory, in its own header so the storage
// backend (vfs_emscripten.cpp) can invalidate it without pulling in the whole
// Win32 compatibility layer.
//
// Every OPFS lookup is a round-trip to the storage worker through Asyncify, and
// the legacy code probes for absent files constantly (File::open tries loose
// files on disk before its fastfiles, by design -- "useful for patches"). These
// tables answer repeat questions from memory. ANY write, create, copy, delete
// or mkdir must call mc2_path_cache_invalidate(), because each of those can turn
// a remembered answer into a wrong one.
#include <cctype>
#include <iterator>
#include <map>
#include <set>
#include <string>

// Exact paths known to be absent.
inline std::set<std::string>& mc2_missing_paths()
{
	static std::set<std::string> table;
	return table;
}
// Exact path -> the case-corrected path that exists (or itself on a miss).
inline std::map<std::string, std::string>& mc2_resolved_paths()
{
	static std::map<std::string, std::string> table;
	return table;
}
// Directory -> (lower-cased entry name -> real entry name). One directory scan
// per directory per session instead of one per missing file under it.
inline std::map<std::string, std::map<std::string, std::string>>& mc2_dir_listings()
{
	static std::map<std::string, std::map<std::string, std::string>> table;
	return table;
}
// Directories known to be absent (no case-insensitive match either). Every
// path beneath one is absent without asking storage.
inline std::set<std::string>& mc2_missing_dirs()
{
	static std::set<std::string> table;
	return table;
}

inline void mc2_path_cache_invalidate()
{
	mc2_resolved_paths().clear();
	mc2_missing_paths().clear();
	mc2_dir_listings().clear();
	mc2_missing_dirs().clear();
}

// M8-B77, measured 2026-09-30: every file the game created (a save's temp
// files, the save itself) cleared ALL of these tables, so the next texture
// lookup re-listed data/textures -- about 630 ms each in the browser, twice per
// quickload. Creating or removing one file changes only (a) the answers for
// that exact path and (b) one name in its parent's listing, so update exactly
// that. Case is ignored: keys hold whatever spelling was asked or resolved.
// Directory creation still uses the full invalidate above.
inline std::string mc2_path_cache_lower(std::string s)
{
	for (char& c : s) c = (char)std::tolower((unsigned char)c);
	return s;
}
inline void mc2_path_cache_forget_exact(const std::string& wanted)
{
	std::set<std::string>& missing = mc2_missing_paths();
	for (auto it = missing.begin(); it != missing.end();)
		it = (mc2_path_cache_lower(*it) == wanted) ? missing.erase(it) : std::next(it);
	std::map<std::string, std::string>& resolved = mc2_resolved_paths();
	for (auto it = resolved.begin(); it != resolved.end();)
		it = (mc2_path_cache_lower(it->first) == wanted || mc2_path_cache_lower(it->second) == wanted)
			? resolved.erase(it) : std::next(it);
}
// A FILE now exists at `posix`.
inline void mc2_path_cache_note_file(const std::string& posix)
{
	const std::string wanted = mc2_path_cache_lower(posix);
	// A file cannot appear under a directory remembered as missing unless
	// directories changed too -- fall back to the full clear.
	for (const std::string& dir : mc2_missing_dirs())
	{
		const std::string prefix = mc2_path_cache_lower(dir) + "/";
		if (wanted.compare(0, prefix.size(), prefix) == 0) { mc2_path_cache_invalidate(); return; }
	}
	mc2_path_cache_forget_exact(wanted);
	const std::size_t slash = posix.rfind('/');
	if (slash == std::string::npos) return;
	const std::string parent = mc2_path_cache_lower(posix.substr(0, slash));
	const std::string name = posix.substr(slash + 1);
	for (auto& listing : mc2_dir_listings())
		if (mc2_path_cache_lower(listing.first) == parent) listing.second[mc2_path_cache_lower(name)] = name;
}
// The FILE at `posix` was removed.
inline void mc2_path_cache_note_removed(const std::string& posix)
{
	const std::string wanted = mc2_path_cache_lower(posix);
	mc2_path_cache_forget_exact(wanted);
	const std::size_t slash = posix.rfind('/');
	if (slash == std::string::npos) return;
	const std::string parent = mc2_path_cache_lower(posix.substr(0, slash));
	const std::string name = mc2_path_cache_lower(posix.substr(slash + 1));
	for (auto& listing : mc2_dir_listings())
		if (mc2_path_cache_lower(listing.first) == parent) listing.second.erase(name);
}

// True when some directory prefix of `posix` is a remembered missing directory.
inline bool mc2_under_missing_dir(const std::string& posix)
{
	const std::set<std::string>& dirs = mc2_missing_dirs();
	if (dirs.empty()) return false;
	for (std::size_t slash = posix.find('/', 1); slash != std::string::npos;
	     slash = posix.find('/', slash + 1))
		if (dirs.count(posix.substr(0, slash))) return true;
	return false;
}
