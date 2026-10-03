//===========================================================================//
// VfsBackend -- storage operations beneath GameOS path-resolution policy.
//
// Paths passed through this interface are already normalized and resolved by
// vfs_resolve.cpp. Backends deal only in storage: they do not rewrite paths,
// perform case folding, or know about GameOS application hooks.
//===========================================================================//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class VfsFile
{
public:
    virtual ~VfsFile() {}
};

struct VfsStat
{
    VfsStat() : exists(false), directory(false), size(0), timestamp(0) {}

    bool          exists;
    bool          directory;
    std::uint64_t size;
    std::int64_t  timestamp;
};

class VfsBackend
{
public:
    virtual ~VfsBackend() {}

    virtual VfsFile* Open(const char* path, const char* mode) = 0;
    virtual void Close(VfsFile* file) = 0;
    virtual std::size_t Read(VfsFile* file, void* buffer, std::size_t size) = 0;
    virtual std::size_t Write(VfsFile* file, const void* buffer, std::size_t size) = 0;
    virtual std::uint64_t Size(VfsFile* file) = 0;

    virtual bool Exists(const char* path) = 0;
    virtual std::uint64_t Size(const char* path) = 0;
    virtual VfsStat Stat(const char* path) = 0;
    virtual bool Enumerate(const char* directory, std::vector<std::string>& entries) = 0;
    virtual bool SetReadWrite(const char* path) = 0;
    virtual bool CreateDirectory(const char* path) = 0;
    virtual void DumpStats() {}
};

VfsBackend& Vfs();
VfsBackend* CreateNativeVfsBackend();
VfsBackend* CreateEmscriptenVfsBackend();
bool InitializeEmscriptenVfs();
