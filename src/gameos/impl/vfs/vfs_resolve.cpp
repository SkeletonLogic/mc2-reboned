//===========================================================================//
// Shared path normalization, case-insensitive fallback, and find filtering.
// Storage access is exclusively through VfsBackend.
//===========================================================================//

#include "vfs_resolve.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <unordered_map>

#include "vfs_backend.h"

namespace {

std::unordered_map<std::string, std::string> g_resolvedPaths;
std::unordered_map<std::string, std::vector<std::string> > g_directoryEntries;
std::unordered_map<std::string, bool> g_directoryPaths;
std::uint64_t g_resolveRequests = 0;
std::uint64_t g_resolveCacheHits = 0;
std::uint64_t g_directoryCacheHits = 0;
std::uint64_t g_directoryEnumerations = 0;
std::uint64_t g_negativeResults = 0;
std::uint64_t g_singleOpenWholeFiles = 0;

std::string toLower(const std::string& value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

} // namespace

std::string VfsNormalizePath(const char* path)
{
    std::string normalized(path ? path : "");
    for (std::size_t i = 0; i < normalized.size(); ++i)
        if (normalized[i] == '\\') normalized[i] = '/';

    // No backend may be handed an EMPTY path component. MC2 builds paths by
    // concatenation and sometimes emits a trailing separator with no file name
    // -- `assets\graphics\` is requested 8 times in a 200-tick mc2_01 run --
    // and it can produce doubled separators the same way. stdio merely fails on
    // those, which is why this was invisible for five milestones; OPFS instead
    // throws `getFileHandle: Name is not allowed`, because "" is not a legal
    // name. Collapsing here keeps the rule in shared policy, where every backend
    // inherits it, rather than papering over it in the browser peer.
    std::string collapsed;
    collapsed.reserve(normalized.size());
    for (std::size_t i = 0; i < normalized.size(); ++i)
    {
        if (normalized[i] == '/' && !collapsed.empty() &&
            collapsed[collapsed.size() - 1] == '/')
            continue;
        collapsed += normalized[i];
    }

    // Keep a bare root ("/") and a drive root ("C:/"); strip any other trailing
    // separator so the final component is always a real name.
    const bool driveRoot = collapsed.size() == 3 && collapsed[1] == ':';
    if (collapsed.size() > 1 && !driveRoot &&
        collapsed[collapsed.size() - 1] == '/')
        collapsed.erase(collapsed.size() - 1);

    return collapsed;
}

std::string VfsResolvePath(const char* path)
{
    const std::string normalized = VfsNormalizePath(path);
    ++g_resolveRequests;
    const auto cached = g_resolvedPaths.find(normalized);
    if (cached != g_resolvedPaths.end())
    {
        ++g_resolveCacheHits;
        return cached->second;
    }
    if (Vfs().Exists(normalized.c_str()))
    {
        g_resolvedPaths[normalized] = normalized;
        return normalized;
    }

    std::vector<std::string> parts;
    std::string current;
    for (std::size_t i = 0; i <= normalized.size(); ++i)
    {
        if (i == normalized.size() || normalized[i] == '/')
        {
            if (!current.empty() || (i > 0 && normalized[i - 1] == ':'))
                parts.push_back(current);
            current.clear();
        }
        else current += normalized[i];
    }
    if (parts.empty()) return normalized;

    std::string built;
    std::size_t start = 0;
    if (!normalized.empty() && normalized[0] == '/') built = "/";
    if (parts[0].size() >= 2 && parts[0][1] == ':')
    {
        built = parts[0] + "/";
        start = 1;
    }

    for (std::size_t i = start; i < parts.size(); ++i)
    {
        std::string candidate = built.empty() ? parts[i] : built + parts[i];
        if (Vfs().Exists(candidate.c_str()))
        {
            built = candidate + (i + 1 < parts.size() ? "/" : "");
            continue;
        }

        std::string parent = built.empty() ? "." : built;
        if (!parent.empty() && parent[parent.size() - 1] == '/')
            parent.erase(parent.size() - 1);

        std::vector<std::string> entries;
        const auto directory = g_directoryEntries.find(parent);
        if (directory != g_directoryEntries.end())
        {
            ++g_directoryCacheHits;
            entries = directory->second;
        }
        else
        {
            ++g_directoryEnumerations;
            if (!Vfs().Enumerate(parent.c_str(), entries))
            {
                ++g_negativeResults;
                g_resolvedPaths[normalized] = normalized;
                return normalized;
            }
            g_directoryEntries[parent] = entries;
        }

        const std::string wanted = toLower(parts[i]);
        bool found = false;
        for (std::size_t entry = 0; entry < entries.size(); ++entry)
        {
            if (toLower(entries[entry]) == wanted)
            {
                candidate = built.empty() ? entries[entry] : built + entries[entry];
                built = candidate + (i + 1 < parts.size() ? "/" : "");
                found = true;
                break;
            }
        }
        if (!found)
        {
            ++g_negativeResults;
            g_resolvedPaths[normalized] = normalized;
            return normalized;
        }
    }
    g_resolvedPaths[normalized] = built;
    return built;
}

bool VfsResolvedIsDirectory(const std::string& resolved)
{
    // MC2's font loader asks gos_GetFile for `assets\graphics\` -- a DIRECTORY --
    // 226 times in a 600-tick browser run. Natively stdio just returns NULL and
    // nobody notices. Over OPFS every one of those is a path lookup, and looking
    // a directory up as a file throws, which is where the "unexpected error"
    // storm came from. Remember the answer so the question is asked once.
    const auto known = g_directoryPaths.find(resolved);
    if (known != g_directoryPaths.end()) return known->second;
    const bool directory = Vfs().Stat(resolved.c_str()).directory;
    g_directoryPaths[resolved] = directory;
    return directory;
}

void VfsInvalidatePathCaches()
{
    g_resolvedPaths.clear();
    g_directoryEntries.clear();
    g_directoryPaths.clear();
}

void VfsNoteWholeFileSingleOpen() { ++g_singleOpenWholeFiles; }

void VfsDumpStats()
{
    std::printf("[vfs] resolve requests=%llu cacheHits=%llu directoryCacheHits=%llu "
                "enumerations=%llu negativeResults=%llu\n",
                (unsigned long long)g_resolveRequests,
                (unsigned long long)g_resolveCacheHits,
                (unsigned long long)g_directoryCacheHits,
                (unsigned long long)g_directoryEnumerations,
                (unsigned long long)g_negativeResults);
    std::printf("[vfs] whole-file single-open reads=%llu avoidedPathReopens=%llu\n",
                (unsigned long long)g_singleOpenWholeFiles,
                (unsigned long long)g_singleOpenWholeFiles);
    Vfs().DumpStats();
}

void VfsFindEntries(const char* pathPattern, bool wantDirectories,
                    std::vector<std::string>& entries)
{
    entries.clear();

    const std::string normalized = VfsNormalizePath(pathPattern);
    std::string directory;
    std::string pattern;
    const std::size_t slash = normalized.find_last_of('/');
    if (slash == std::string::npos)
    {
        directory = ".";
        pattern = normalized;
    }
    else
    {
        directory = normalized.substr(0, slash);
        pattern = normalized.substr(slash + 1);
    }
    if (directory.empty()) directory = "/";

    const std::string realDirectory = VfsResolvePath(directory.c_str());
    std::vector<std::string> all;
    if (!Vfs().Enumerate(realDirectory.c_str(), all)) return;

    std::string extension;
    const std::size_t dot = pattern.find_last_of('.');
    if (!pattern.empty() && pattern[0] == '*' && dot != std::string::npos)
        extension = toLower(pattern.substr(dot));

    for (std::size_t i = 0; i < all.size(); ++i)
    {
        if (all[i] == "." || all[i] == "..") continue;

        const std::string full = realDirectory + "/" + all[i];
        if (Vfs().Stat(full.c_str()).directory != wantDirectories) continue;

        if (!extension.empty())
        {
            const std::string lower = toLower(all[i]);
            if (lower.size() < extension.size() ||
                lower.compare(lower.size() - extension.size(), extension.size(), extension) != 0)
                continue;
        }
        entries.push_back(all[i]);
    }
}
