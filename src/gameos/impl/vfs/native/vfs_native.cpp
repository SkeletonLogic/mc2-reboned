//===========================================================================//
// Native stdio/filesystem VFS backend. This file contains storage mechanics
// only; separator normalization and case fallback live in vfs_resolve.cpp.
//===========================================================================//

#include "../vfs_backend.h"

#include <cstdio>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class NativeVfsFile : public VfsFile
{
public:
    explicit NativeVfsFile(std::FILE* value) : file(value) {}
    std::FILE* file;
};

class NativeVfsBackend : public VfsBackend
{
public:
    VfsFile* Open(const char* path, const char* mode) override
    {
        std::FILE* file = std::fopen(path, mode);
        return file ? new NativeVfsFile(file) : nullptr;
    }

    void Close(VfsFile* file) override
    {
        NativeVfsFile* native = static_cast<NativeVfsFile*>(file);
        if (!native) return;
        if (native->file) std::fclose(native->file);
        delete native;
    }

    std::size_t Read(VfsFile* file, void* buffer, std::size_t size) override
    {
        NativeVfsFile* native = static_cast<NativeVfsFile*>(file);
        return native && native->file ? std::fread(buffer, 1, size, native->file) : 0;
    }

    std::size_t Write(VfsFile* file, const void* buffer, std::size_t size) override
    {
        NativeVfsFile* native = static_cast<NativeVfsFile*>(file);
        return native && native->file ? std::fwrite(buffer, 1, size, native->file) : 0;
    }

    std::uint64_t Size(VfsFile* file) override
    {
        NativeVfsFile* native = static_cast<NativeVfsFile*>(file);
        if (!native || !native->file) return 0;
        const long position = std::ftell(native->file);
        if (std::fseek(native->file, 0, SEEK_END) != 0) return 0;
        const long size = std::ftell(native->file);
        if (position >= 0) std::fseek(native->file, position, SEEK_SET);
        return size < 0 ? 0 : static_cast<std::uint64_t>(size);
    }

    bool Exists(const char* path) override
    {
#if defined(_WIN32)
        return _access(path, 0) == 0;
#else
        struct stat info;
        return stat(path, &info) == 0;
#endif
    }

    std::uint64_t Size(const char* path) override
    {
        std::FILE* file = std::fopen(path, "rb");
        if (!file) return 0;
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fclose(file);
        return size < 0 ? 0 : static_cast<std::uint64_t>(size);
    }

    VfsStat Stat(const char* path) override
    {
        VfsStat result;
#if defined(_WIN32)
        struct _stat info;
        if (_stat(path, &info) != 0) return result;
        result.directory = (info.st_mode & _S_IFDIR) != 0;
#else
        struct stat info;
        if (stat(path, &info) != 0) return result;
        result.directory = S_ISDIR(info.st_mode);
#endif
        result.exists = true;
        result.size = info.st_size < 0 ? 0 : static_cast<std::uint64_t>(info.st_size);
        result.timestamp = static_cast<std::int64_t>(info.st_mtime);
        return result;
    }

    bool Enumerate(const char* directory, std::vector<std::string>& entries) override
    {
        entries.clear();
#if defined(_WIN32)
        const std::string pattern = !directory || !*directory
            ? "*" : (std::string(directory) + "/*");
        struct _finddata_t data;
        const intptr_t handle = _findfirst(pattern.c_str(), &data);
        if (handle == -1) return false;
        do { entries.push_back(data.name); } while (_findnext(handle, &data) == 0);
        _findclose(handle);
        return true;
#else
        DIR* dir = opendir(!directory || !*directory ? "." : directory);
        if (!dir) return false;
        while (struct dirent* entry = readdir(dir)) entries.push_back(entry->d_name);
        closedir(dir);
        return true;
#endif
    }

    bool SetReadWrite(const char* path) override
    {
#if defined(_WIN32)
        return _chmod(path, _S_IREAD | _S_IWRITE) == 0;
#else
        return chmod(path, 0644) == 0;
#endif
    }

    bool CreateDirectory(const char* path) override
    {
#if defined(_WIN32)
        return _mkdir(path) == 0;
#else
        return mkdir(path, 0755) == 0;
#endif
    }
};

} // namespace

VfsBackend* CreateNativeVfsBackend()
{
    static NativeVfsBackend backend;
    return &backend;
}
