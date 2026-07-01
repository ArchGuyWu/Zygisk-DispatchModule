#include "pointer_sanitizer.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

namespace {

struct VmaEntry {
    uintptr_t start;
    uintptr_t end;
    bool readable;
};

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

constexpr uint64_t kVmaTtlNs = 250000000ULL; // 250 ms

std::shared_mutex g_vma_mutex;
std::vector<VmaEntry> g_vma_entries;
std::atomic<uint64_t> g_vma_last_refresh_ns{0};

uint64_t monotonic_ns() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

bool vma_lookup_unlocked(uintptr_t addr) {
    const auto& entries = g_vma_entries;
    if (entries.empty()) return false;

    size_t lo = 0;
    size_t hi = entries.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (entries[mid].end <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= entries.size()) return false;
    const VmaEntry& e = entries[lo];
    return addr >= e.start && addr < e.end && e.readable;
}

bool parse_maps_file(std::vector<VmaEntry>& out) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;

    char line[512];
    std::vector<VmaEntry> parsed;
    parsed.reserve(512);

    while (fgets(line, sizeof(line), fp)) {
        uintptr_t start = 0;
        uintptr_t end = 0;
        char perms[8] = {};
        unsigned long long start_raw = 0;
        unsigned long long end_raw = 0;
        if (std::sscanf(line, "%llx-%llx %7s", &start_raw, &end_raw, perms) < 3) {
            continue;
        }
        start = static_cast<uintptr_t>(start_raw);
        end = static_cast<uintptr_t>(end_raw);
        if (end <= start) continue;
        VmaEntry e{};
        e.start = start;
        e.end = end;
        e.readable = (perms[0] == 'r');
        parsed.push_back(e);
    }
    fclose(fp);

    if (parsed.empty()) return false;
    out.swap(parsed);
    return true;
}

void refresh_vma_cache_locked(uint64_t now_ns) {
    std::vector<VmaEntry> fresh;
    if (!parse_maps_file(fresh)) return;
    g_vma_entries.swap(fresh);
    g_vma_last_refresh_ns.store(now_ns, std::memory_order_release);
}

void refresh_vma_cache_if_stale() {
    uint64_t now = monotonic_ns();
    uint64_t last = g_vma_last_refresh_ns.load(std::memory_order_acquire);
    if (last != 0 && now > last && (now - last) < kVmaTtlNs) {
        return;
    }

    std::unique_lock lock(g_vma_mutex);
    last = g_vma_last_refresh_ns.load(std::memory_order_relaxed);
    if (last != 0 && now > last && (now - last) < kVmaTtlNs) {
        return;
    }
    refresh_vma_cache_locked(now);
}

} // namespace

bool pipe_probe_readable(const void* ptr) {
    if (!is_userspace_address(ptr)) return false;

    thread_local static ThreadLocalPipe tl_pipe;
    if (tl_pipe.fds[1] < 0) return false;

    long ret = write(tl_pipe.fds[1], ptr, 1);
    if (ret >= 0) {
        char dummy;
        (void)read(tl_pipe.fds[0], &dummy, 1);
        return true;
    }
    return errno == EFAULT ? false : false;
}

bool vma_is_readable(const void* ptr) {
    if (!is_userspace_address(ptr)) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    refresh_vma_cache_if_stale();

    std::shared_lock lock(g_vma_mutex);
    return vma_lookup_unlocked(addr);
}

bool is_pointer_readable_strict(const void* ptr) {
    return pipe_probe_readable(ptr);
}

bool is_pointer_readable(const void* ptr) {
    // Tier 1
    if (!is_userspace_address(ptr)) return false;

    // Tier 2 — hot path
    if (vma_is_readable(ptr)) return true;

    // Tier 3 — maps miss: stale cache, just-mapped region, or truly invalid
    if (pipe_probe_readable(ptr)) {
        // Pipe succeeded but VMA missed → refresh once and prefer cache next time.
        uint64_t now = monotonic_ns();
        std::unique_lock lock(g_vma_mutex);
        refresh_vma_cache_locked(now);
        return true;
    }
    return false;
}

namespace {

bool vm_read_memory(const void* src, void* dst, size_t len) {
    if (!src || !dst || len == 0) return false;
    if (!is_userspace_address(src)) return false;
    struct iovec local_iov {};
    struct iovec remote_iov {};
    local_iov.iov_base = dst;
    local_iov.iov_len = len;
    remote_iov.iov_base = const_cast<void*>(src);
    remote_iov.iov_len = len;
    const ssize_t n = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 0);
    return n == static_cast<ssize_t>(len);
}

} // namespace

bool safe_read_u16(const void* addr, uint16_t* out) {
    if (!addr || !out) return false;
    uint16_t buf = 0;
    if (!vm_read_memory(addr, &buf, sizeof(buf))) return false;
    *out = buf;
    return true;
}

int32_t safe_utf16_strlen_bounded(const void* s, int32_t max_units) {
    if (!s || max_units <= 0) return 0;
    if (!is_userspace_address(s)) return 0;
    if (is_probable_stale_icu_string_ptr(s)) return 0;
    if (!vma_is_readable(s)) return 0;

    for (int32_t i = 0; i < max_units; ++i) {
        const void* unit_addr =
            reinterpret_cast<const uint16_t*>(s) + i;
        if (!vma_is_readable(unit_addr)) return 0;
        uint16_t ch = 0;
        if (!safe_read_u16(unit_addr, &ch)) return 0;
        if (ch == 0) return i;
    }
    return 0;
}