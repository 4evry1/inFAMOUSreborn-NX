#ifndef PS3RECOMP_HOST_FILE_H
#define PS3RECOMP_HOST_FILE_H

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace ps3recomp::host {

inline std::mutex& append_mutex()
{
    static std::mutex mutex;
    return mutex;
}

enum class FileAccess {
    read_only,
    write_only,
    read_write,
};

struct OpenOptions {
    FileAccess access = FileAccess::read_only;
    bool create = false;
    bool truncate = false;
    bool append = false;
};

class File final {
public:
    static std::shared_ptr<File> open(const char* path, const OpenOptions& options,
                                      int* host_error = nullptr)
    {
        auto file = std::shared_ptr<File>(new File());

#ifdef _WIN32
        DWORD access = 0;
        if (options.access != FileAccess::write_only)
            access |= GENERIC_READ;
        if (options.access != FileAccess::read_only) {
            access |= GENERIC_WRITE;
            if (options.append)
                access |= FILE_APPEND_DATA;
        }

        DWORD disposition = OPEN_EXISTING;
        if (options.truncate && options.create)
            disposition = CREATE_ALWAYS;
        else if (options.truncate)
            disposition = TRUNCATE_EXISTING;
        else if (options.create)
            disposition = OPEN_ALWAYS;

        file->handle_ = CreateFileA(
            path, access,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (file->handle_ == INVALID_HANDLE_VALUE) {
            const int error = (int)GetLastError();
            if (host_error)
                *host_error = error;
            return nullptr;
        }
#else
        int flags = 0;
        if (options.access == FileAccess::read_only)
            flags = O_RDONLY;
        else if (options.access == FileAccess::write_only)
            flags = O_WRONLY;
        else
            flags = O_RDWR;
        if (options.create)
            flags |= O_CREAT;
        if (options.truncate)
            flags |= O_TRUNC;
        if (options.append)
            flags |= O_APPEND;
        flags |= O_CLOEXEC;
#  ifdef O_BINARY
        flags |= O_BINARY;
#  endif

        file->fd_ = ::open(path, flags, 0666);
        if (file->fd_ < 0) {
            const int error = errno;
            if (host_error)
                *host_error = error;
            return nullptr;
        }
#endif
        file->append_ = options.append;
        return file;
    }

    static std::shared_ptr<File> temporary(int* host_error = nullptr)
    {
        auto file = std::shared_ptr<File>(new File());

#ifdef _WIN32
        char directory[MAX_PATH + 1] = {};
        char path[MAX_PATH + 1] = {};
        const DWORD directory_length =
            GetTempPathA((DWORD)sizeof(directory), directory);
        if (directory_length == 0 ||
            directory_length >= sizeof(directory) ||
            GetTempFileNameA(directory, "p3r", 0, path) == 0) {
            const int error = (int)GetLastError();
            if (host_error)
                *host_error = error;
            return nullptr;
        }
        file->handle_ = CreateFileA(
            path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
            nullptr);
        if (file->handle_ == INVALID_HANDLE_VALUE) {
            const int error = (int)GetLastError();
            DeleteFileA(path);
            if (host_error)
                *host_error = error;
            return nullptr;
        }
#else
        char path[] = "/tmp/ps3recomp-host-file-XXXXXX";
        file->fd_ = ::mkstemp(path);
        if (file->fd_ < 0) {
            const int error = errno;
            if (host_error)
                *host_error = error;
            return nullptr;
        }
        ::unlink(path);
#endif
        file->append_ = false;
        return file;
    }

    ~File()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
#else
        if (fd_ >= 0)
            ::close(fd_);
#endif
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    std::size_t read(void* buffer, std::size_t size)
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return read_unlocked(buffer, size);
    }

    std::size_t read_at(std::uint64_t offset, void* buffer, std::size_t size)
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
#ifdef _WIN32
        LARGE_INTEGER saved = {};
        LARGE_INTEGER target = {};
        target.QuadPart = (LONGLONG)offset;
        if (!SetFilePointerEx(handle_, LARGE_INTEGER{}, &saved, FILE_CURRENT) ||
            !SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN)) {
            record_windows_error();
            return 0;
        }
        const std::size_t result = read_unlocked(buffer, size);
        if (!SetFilePointerEx(handle_, saved, nullptr, FILE_BEGIN))
            record_windows_error();
        return result;
#else
        std::size_t total = 0;
        while (total < size) {
            const ssize_t result = ::pread(
                fd_, static_cast<unsigned char*>(buffer) + total,
                size - total, (off_t)(offset + total));
            if (result > 0) {
                total += (std::size_t)result;
                continue;
            }
            if (result < 0 && errno == EINTR)
                continue;
            if (result < 0)
                last_error_.store(errno, std::memory_order_relaxed);
            break;
        }
        return total;
#endif
    }

    std::size_t write(const void* buffer, std::size_t size)
    {
        std::unique_lock<std::mutex> handle_lock(io_mutex_);
        std::unique_lock<std::mutex> append_lock(append_mutex(), std::defer_lock);
        if (append_)
            append_lock.lock();
#ifdef _WIN32
        if (append_) {
            LARGE_INTEGER end = {};
            if (!SetFilePointerEx(handle_, LARGE_INTEGER{}, &end, FILE_END)) {
                record_windows_error();
                return 0;
            }
        }
        std::size_t total = 0;
        while (total < size) {
            const DWORD chunk = (DWORD)std::min<std::size_t>(
                size - total, (std::size_t)(std::numeric_limits<DWORD>::max)());
            DWORD written = 0;
            if (!WriteFile(handle_, static_cast<const unsigned char*>(buffer) + total,
                           chunk, &written, nullptr)) {
                record_windows_error();
                break;
            }
            total += written;
            if (written != chunk)
                break;
        }
        if (append_ && total) {
            LARGE_INTEGER end = {};
            if (!SetFilePointerEx(handle_, LARGE_INTEGER{}, &end, FILE_END))
                record_windows_error();
        }
        return total;
#else
        std::size_t total = 0;
        while (total < size) {
            const ssize_t result = ::write(
                fd_, static_cast<const unsigned char*>(buffer) + total,
                size - total);
            if (result > 0) {
                total += (std::size_t)result;
                continue;
            }
            if (result < 0 && errno == EINTR)
                continue;
            if (result < 0)
                last_error_.store(errno, std::memory_order_relaxed);
            break;
        }
        return total;
#endif
    }

    bool seek(std::int64_t offset, int whence)
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
#ifdef _WIN32
        DWORD method = FILE_BEGIN;
        if (whence == SEEK_CUR)
            method = FILE_CURRENT;
        else if (whence == SEEK_END)
            method = FILE_END;
        LARGE_INTEGER distance = {};
        distance.QuadPart = (LONGLONG)offset;
        if (!SetFilePointerEx(handle_, distance, nullptr, method)) {
            record_windows_error();
            return false;
        }
        return true;
#else
        if (::lseek(fd_, (off_t)offset, whence) < 0) {
            last_error_.store(errno, std::memory_order_relaxed);
            return false;
        }
        return true;
#endif
    }

    std::int64_t position()
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return position_unlocked();
    }

    std::uint64_t size()
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
#ifdef _WIN32
        LARGE_INTEGER result = {};
        if (!GetFileSizeEx(handle_, &result)) {
            record_windows_error();
            return 0;
        }
        return (std::uint64_t)result.QuadPart;
#else
        struct stat result = {};
        if (::fstat(fd_, &result) != 0) {
            last_error_.store(errno, std::memory_order_relaxed);
            return 0;
        }
        return (std::uint64_t)result.st_size;
#endif
    }

    bool flush()
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) {
            record_windows_error();
            return false;
        }
#else
        if (::fsync(fd_) != 0) {
            last_error_.store(errno, std::memory_order_relaxed);
            return false;
        }
#endif
        return true;
    }

    int last_host_error() const
    {
        return last_error_.load(std::memory_order_relaxed);
    }

private:
    File() = default;

#ifdef _WIN32
    std::size_t read_unlocked(void* buffer, std::size_t size)
    {
        std::size_t total = 0;
        while (total < size) {
            const DWORD chunk = (DWORD)std::min<std::size_t>(
                size - total, (std::size_t)(std::numeric_limits<DWORD>::max)());
            DWORD received = 0;
            if (!ReadFile(handle_, static_cast<unsigned char*>(buffer) + total,
                          chunk, &received, nullptr)) {
                record_windows_error();
                break;
            }
            total += received;
            if (received != chunk)
                break;
        }
        return total;
    }

    void record_windows_error()
    {
        last_error_.store((int)GetLastError(), std::memory_order_relaxed);
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    std::size_t read_unlocked(void* buffer, std::size_t size)
    {
        std::size_t total = 0;
        while (total < size) {
            const ssize_t result = ::read(
                fd_, static_cast<unsigned char*>(buffer) + total, size - total);
            if (result > 0) {
                total += (std::size_t)result;
                continue;
            }
            if (result < 0 && errno == EINTR)
                continue;
            if (result < 0)
                last_error_.store(errno, std::memory_order_relaxed);
            break;
        }
        return total;
    }

    std::int64_t position_unlocked()
    {
        const off_t result = ::lseek(fd_, 0, SEEK_CUR);
        if (result < 0) {
            last_error_.store(errno, std::memory_order_relaxed);
            return -1;
        }
        return (std::int64_t)result;
    }

    int fd_ = -1;
#endif

#ifdef _WIN32
    std::int64_t position_unlocked()
    {
        LARGE_INTEGER zero = {};
        LARGE_INTEGER current = {};
        if (!SetFilePointerEx(handle_, zero, &current, FILE_CURRENT)) {
            record_windows_error();
            return -1;
        }
        return (std::int64_t)current.QuadPart;
    }
#endif

    std::mutex io_mutex_;
    std::atomic<int> last_error_{0};
    bool append_ = false;
};

} // namespace ps3recomp::host

#endif
