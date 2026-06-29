#pragma once

#include <cstdint>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

// POSIX pipe-based pointer readability check (EFAULT on invalid pages).
struct ThreadLocalPipe {
    int fds[2] = {-1, -1};
    ThreadLocalPipe() {
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
            fds[0] = -1;
            fds[1] = -1;
        }
    }
    ~ThreadLocalPipe() {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
};

static inline bool is_pointer_readable(const void* ptr) {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000ULL || addr > 0x00007fffffffffffULL) {
        return false;
    }
    thread_local static ThreadLocalPipe tl_pipe;
    if (tl_pipe.fds[1] < 0) {
        return false;
    }
    long ret = write(tl_pipe.fds[1], ptr, 1);
    if (ret >= 0) {
        char dummy;
        read(tl_pipe.fds[0], &dummy, 1);
        return true;
    }
    if (errno == EFAULT) {
        return false;
    }
    return false;
}