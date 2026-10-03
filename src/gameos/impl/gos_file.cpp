//===========================================================================//
// GameOS file entry points -- thin policy layer over VfsBackend.
//
// Clean-room reimplementation from Source/GameOS/include/ToolOS.hpp and
// GameOS.HPP. Path normalization and case-insensitive fallback are shared in
// vfs_resolve.cpp; storage mechanics live in the selected VFS backend.
//===========================================================================//

#include <cstdio>
#include <string>
#include <vector>

#include <gameos.hpp>
#include <toolos.hpp>

#include "vfs/vfs_backend.h"
#include "vfs/vfs_resolve.h"

struct gosFileStream
{
    VfsFile*   file;
    std::string path;
    bool        writable;
};

namespace {

struct FindState
{
    std::vector<std::string> entries;
    std::size_t              index;
    char                     current[512];
};

FindState g_findFiles = { std::vector<std::string>(), 0, {0} };
FindState g_findDirs  = { std::vector<std::string>(), 0, {0} };

char* advance(FindState& state)
{
    if (state.index >= state.entries.size()) return nullptr;
    std::snprintf(state.current, sizeof(state.current), "%s",
                  state.entries[state.index].c_str());
    ++state.index;
    return state.current;
}

void beginFind(FindState& state, const char* pathPattern, bool wantDirectories)
{
    state.entries.clear();
    state.index = 0;
    VfsFindEntries(pathPattern, wantDirectories, state.entries);
}

} // namespace

void __stdcall gos_OpenFile(HGOSFILE* hfile, const char* path,
                            gosEnum_FileWriteStatus status)
{
    if (!hfile) return;
    *hfile = nullptr;
    if (!path) return;

    const char* mode = "rb";
    switch (status)
    {
        case READONLY:  mode = "rb";  break;
        case READWRITE: mode = "w+b"; break;
        case APPEND:    mode = "a+b"; break;
        default:        mode = "rb";  break;
    }

    const std::string real = status == READONLY
        ? VfsResolvePath(path) : VfsNormalizePath(path);
    VfsFile* file = Vfs().Open(real.c_str(), mode);
    if (!file && status != READONLY)
        file = Vfs().Open(VfsNormalizePath(path).c_str(), mode);
    if (!file) return;
    if (status != READONLY) VfsInvalidatePathCaches();

    gosFileStream* stream = new gosFileStream();
    stream->file = file;
    stream->path = real;
    stream->writable = status != READONLY;
    *hfile = stream;
}

void __stdcall gos_CloseFile(HGOSFILE hfile)
{
    if (!hfile) return;
    Vfs().Close(hfile->file);
    delete hfile;
}

DWORD __stdcall gos_ReadFile(HGOSFILE hfile, void* buffer, DWORD size)
{
    if (!hfile || !hfile->file || !buffer || size == 0) return 0;
    return static_cast<DWORD>(Vfs().Read(hfile->file, buffer, size));
}

DWORD __stdcall gos_WriteFile(HGOSFILE hfile, const void* buffer, DWORD size)
{
    if (!hfile || !hfile->file || !buffer || size == 0) return 0;
    VfsInvalidatePathCaches();
    return static_cast<DWORD>(Vfs().Write(hfile->file, buffer, size));
}

bool __stdcall gos_DoesFileExist(const char* fileName)
{
    if (!fileName) return false;
    if (Environment.HookDoesFileExist)
        return Environment.HookDoesFileExist(fileName);
    return Vfs().Exists(VfsResolvePath(fileName).c_str());
}

DWORD __stdcall gos_FileSize(const char* fileName)
{
    if (!fileName) return 0;
    return static_cast<DWORD>(Vfs().Size(VfsResolvePath(fileName).c_str()));
}

__int64 __stdcall gos_FileTimeStamp(const char* fileName)
{
    if (!fileName) return 0;
    return static_cast<__int64>(Vfs().Stat(VfsResolvePath(fileName).c_str()).timestamp);
}

void __stdcall gos_FileSetReadWrite(const char* fileName)
{
    if (fileName) Vfs().SetReadWrite(VfsResolvePath(fileName).c_str());
}

bool __stdcall gos_CreateDirectory(const char* fileName)
{
    if (!fileName) return false;
    const std::string path = VfsNormalizePath(fileName);

    for (std::size_t i = 1; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            const std::string part = path.substr(0, i);
            if (part.empty() || (part.size() == 2 && part[1] == ':')) continue;
            if (!Vfs().Stat(part.c_str()).directory) Vfs().CreateDirectory(part.c_str());
        }
    }
    const bool created = Vfs().Stat(path.c_str()).directory;
    if (created) VfsInvalidatePathCaches();
    return created;
}

void __stdcall gos_GetFile(const char* fileName, BYTE** memoryImage, DWORD* size)
{
    if (memoryImage) *memoryImage = nullptr;
    if (size) *size = 0;
    if (!fileName) return;

    if (Environment.HookGetFile)
    {
        Environment.HookGetFile(fileName, memoryImage, size);
        if (memoryImage && *memoryImage) return;
    }

    const std::string real = VfsResolvePath(fileName);

    // A whole-file read of a directory is meaningless, and MC2 asks for one on
    // every font load. stdio answers NULL; OPFS pays a throwing path lookup.
    // Answer it from the shared cache instead of the storage layer.
    if (VfsResolvedIsDirectory(real)) return;

    VfsFile* file = Vfs().Open(real.c_str(), "rb");
    if (!file) return;

    // PORT: OPFS calls are asyncified; use the already-open handle instead of
    // paying for a separate path-size open before every whole-file read.
    const std::uint64_t byteCount = Vfs().Size(file);
    if (byteCount == 0)
    {
        Vfs().Close(file);
        return;
    }
    VfsNoteWholeFileSingleOpen();

    BYTE* buffer = static_cast<BYTE*>(gos_Malloc(static_cast<std::size_t>(byteCount)));
    if (!buffer)
    {
        Vfs().Close(file);
        return;
    }

    const std::size_t read = Vfs().Read(file, buffer, static_cast<std::size_t>(byteCount));
    Vfs().Close(file);

    if (memoryImage) *memoryImage = buffer;
    if (size) *size = static_cast<DWORD>(read);
}

char* __stdcall gos_FindFiles(const char* pathFileName)
{
    beginFind(g_findFiles, pathFileName, false);
    return advance(g_findFiles);
}

char* __stdcall gos_FindFilesNext() { return advance(g_findFiles); }

void __stdcall gos_FindFilesClose()
{
    g_findFiles.entries.clear();
    g_findFiles.index = 0;
}

char* __stdcall gos_FindDirectories(const char* directoryName)
{
    beginFind(g_findDirs, directoryName, true);
    return advance(g_findDirs);
}

char* __stdcall gos_FindDirectoriesNext() { return advance(g_findDirs); }

void __stdcall gos_FindDirectoriesClose()
{
    g_findDirs.entries.clear();
    g_findDirs.index = 0;
}
