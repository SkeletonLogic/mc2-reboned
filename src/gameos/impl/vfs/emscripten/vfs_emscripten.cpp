//===========================================================================//
// PORT: M5-5 Emscripten OPFS backend. WasmFS presents synchronous POSIX calls
// to legacy GameOS while Asyncify bridges the browser's asynchronous OPFS API.
//===========================================================================//

#include "../vfs_backend.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <emscripten/wasmfs.h>
#include "../../../compat/mc2_path_cache.h"

namespace {

double g_storageMilliseconds = 0.0;
std::uint64_t g_storageCalls = 0;
std::uint64_t g_storageReadBytes = 0;

class StorageTimer
{
public:
    StorageTimer() : start(std::chrono::steady_clock::now()) { ++g_storageCalls; }
    ~StorageTimer()
    {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        g_storageMilliseconds += std::chrono::duration<double, std::milli>(elapsed).count();
    }
private:
    std::chrono::steady_clock::time_point start;
};

std::string PosixPath(const char* path)
{
    std::string result = path ? path : "";
    for (char& value : result) if (value == '\\') value = '/';
    return result;
}

class OpfsFile final : public VfsFile
{
public:
    explicit OpfsFile(std::FILE* value) : file(value) {}
    std::FILE* file;
};

class OpfsBackend final : public VfsBackend
{
public:
    VfsFile* Open(const char* path, const char* mode) override
    {
        StorageTimer timer;
        const std::string resolved = PosixPath(path);
        std::FILE* file = std::fopen(resolved.c_str(), mode);
        // A write can create a file the lookup cache remembers as missing.
        if (file && mode && (std::strpbrk(mode, "wa+") != nullptr)) mc2_path_cache_note_file(resolved);   // M8-B77
        return file ? new OpfsFile(file) : nullptr;
    }
    void Close(VfsFile* file) override
    {
        StorageTimer timer;
        OpfsFile* opfs = static_cast<OpfsFile*>(file);
        if (!opfs) return;
        if (opfs->file) std::fclose(opfs->file);
        delete opfs;
    }
    std::size_t Read(VfsFile* file, void* buffer, std::size_t size) override
    {
        StorageTimer timer;
        OpfsFile* opfs = static_cast<OpfsFile*>(file);
        const std::size_t read = opfs && opfs->file ? std::fread(buffer, 1, size, opfs->file) : 0;
        g_storageReadBytes += read;
        return read;
    }
    std::size_t Write(VfsFile* file, const void* buffer, std::size_t size) override
    {
        StorageTimer timer;
        OpfsFile* opfs = static_cast<OpfsFile*>(file);
        return opfs && opfs->file ? std::fwrite(buffer, 1, size, opfs->file) : 0;
    }
    std::uint64_t Size(VfsFile* file) override
    {
        StorageTimer timer;
        OpfsFile* opfs = static_cast<OpfsFile*>(file);
        if (!opfs || !opfs->file) return 0;
        const long position = std::ftell(opfs->file);
        if (std::fseek(opfs->file, 0, SEEK_END) != 0) return 0;
        const long size = std::ftell(opfs->file);
        if (position >= 0) std::fseek(opfs->file, position, SEEK_SET);
        return size < 0 ? 0 : static_cast<std::uint64_t>(size);
    }
    bool Exists(const char* path) override
    {
        StorageTimer timer;
        struct stat info;
        const std::string resolved = PosixPath(path);
        return stat(resolved.c_str(), &info) == 0;
    }
    std::uint64_t Size(const char* path) override
    {
        StorageTimer timer;
        struct stat info;
        const std::string resolved = PosixPath(path);
        return stat(resolved.c_str(), &info) == 0 && info.st_size > 0
            ? static_cast<std::uint64_t>(info.st_size) : 0;
    }
    VfsStat Stat(const char* path) override
    {
        StorageTimer timer;
        VfsStat result;
        struct stat info;
        const std::string resolved = PosixPath(path);
        if (stat(resolved.c_str(), &info) != 0) return result;
        result.exists = true;
        result.directory = S_ISDIR(info.st_mode);
        result.size = info.st_size < 0 ? 0 : static_cast<std::uint64_t>(info.st_size);
        result.timestamp = static_cast<std::int64_t>(info.st_mtime);
        return result;
    }
    bool Enumerate(const char* directory, std::vector<std::string>& entries) override
    {
        StorageTimer timer;
        entries.clear();
        const std::string resolved = PosixPath(!directory || !*directory ? "." : directory);
        DIR* dir = opendir(resolved.c_str());
        if (!dir) return false;
        while (struct dirent* entry = readdir(dir)) entries.push_back(entry->d_name);
        closedir(dir);
        return true;
    }
    bool SetReadWrite(const char* path) override
    {
        StorageTimer timer;
        return chmod(PosixPath(path).c_str(), 0644) == 0;
    }
    bool CreateDirectory(const char* path) override
    {
        StorageTimer timer;
        const bool made = mkdir(PosixPath(path).c_str(), 0755) == 0;
        if (made) mc2_path_cache_invalidate();
        return made;
    }
    void DumpStats() override
    {
        // PORT: This is measured backend wall time, including synchronous
        // WasmFS/OPFS bridging, rather than the resolver's CPU-only work.
        std::printf("[vfs] storage calls=%llu readBytes=%llu elapsedMs=%.3f\n",
                    (unsigned long long)g_storageCalls,
                    (unsigned long long)g_storageReadBytes,
                    g_storageMilliseconds);
    }
};

} // namespace

VfsBackend* CreateEmscriptenVfsBackend()
{
    static OpfsBackend backend;
    return &backend;
}

bool InitializeEmscriptenVfs()
{
    // PORT: The shell populates the origin-private root before main(). Mount
    // that same root at /mc2 and preserve the game's relative-path assumptions.
    backend_t opfs = wasmfs_create_opfs_backend();
    if (!opfs || wasmfs_create_directory("/mc2", 0755, opfs) != 0 || chdir("/mc2") != 0)
    {
        std::fprintf(stderr, "[vfs] failed to mount OPFS at /mc2\n");
        return false;
    }
    std::printf("[vfs] OPFS mounted at /mc2\n");
    return true;
}
