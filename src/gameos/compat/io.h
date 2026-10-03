#pragma once
// PORT: POSIX-backed subset of the Microsoft low-level file API used by MCLib.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_BINARY 0
#define _S_IREAD S_IRUSR
#define _S_IWRITE S_IWUSR
#define _stat stat
#define _open mc2_open
#if defined(__EMSCRIPTEN__)
// Small read-only files are served from memory; see mc2_read_cache_attach.
#define _close mc2_close
#define _read mc2_read
#define _lseek mc2_lseek
#define _write mc2_write
#else
#define _close ::close
#define _read ::read
#define _lseek ::lseek
#define _write ::write
#endif
#define _access mc2_access
#define _fileno fileno
#define _creat mc2_creat
#define _fstat ::fstat

static int mc2_fmode = 0;
#define _fmode mc2_fmode

inline long _filelength(int descriptor)
{
#if defined(__EMSCRIPTEN__)
	return mc2_filelength(descriptor);
#endif
	struct stat info {};
	return ::fstat(descriptor, &info) == 0 ? static_cast<long>(info.st_size) : -1L;
}
