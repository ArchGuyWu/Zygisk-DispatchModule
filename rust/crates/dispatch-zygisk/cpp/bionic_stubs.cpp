#include <android/log.h>
#include <dlfcn.h>

#include <cstdint>

namespace scudo {
__attribute__((weak)) unsigned computeHardwareCRC32(unsigned, unsigned long) {
    return 0;
}
}  // namespace scudo

extern "C" {
__attribute__((weak)) void __scudo_default_options(void) {}
__attribute__((weak)) void __loader_remove_thread_local_dtor(void* dtor) {
    (void)dtor;
}

void __clear_cache(void* start, void* end) {
    __builtin___clear_cache(static_cast<char*>(start), static_cast<char*>(end));
}

using getauxval_fn = std::uint64_t (*)(std::uint64_t);

std::uint64_t __wrap_getauxval(std::uint64_t type) {
    static getauxval_fn real = nullptr;
    if (!real) {
        real = reinterpret_cast<getauxval_fn>(dlsym(RTLD_DEFAULT, "getauxval"));
    }
    return real ? real(type) : 0;
}
}