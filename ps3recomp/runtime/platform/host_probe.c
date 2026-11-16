#include "host_probe.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

int ps3_host_probe_prefix(const char* path, unsigned char* out, size_t size)
{
    if (!path || !out || !size)
        return -1;

#ifdef _WIN32
    HANDLE file = CreateFileA(
        path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        NULL);
    if (file == INVALID_HANDLE_VALUE)
        return -1;

    DWORD received = 0;
    const DWORD wanted = size > MAXDWORD ? MAXDWORD : (DWORD)size;
    const BOOL ok = ReadFile(file, out, wanted, &received, NULL);
    CloseHandle(file);
    return ok ? (int)received : -1;
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    int file = open(path, flags);
    if (file < 0)
        return -1;

    size_t total = 0;
    while (total < size) {
        ssize_t received = read(file, out + total, size - total);
        if (received > 0) {
            total += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0)
            total = (size_t)-1;
        break;
    }
    close(file);
    return total == (size_t)-1 ? -1 : (int)total;
#endif
}
