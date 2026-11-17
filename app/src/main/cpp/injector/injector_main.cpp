// Hozin Sniper 3D Injector — ptrace remote dlopen (arm64).
//
// Pola root-injector (terbukti di PvZ v1.4 / TR / HCR): attach ke PID
// game, resolve alamat dlopen/mmap remote lewat offset lokal (ASLR
// hanya menggeser base), alokasi scratch di proses game, tulis path
// payload, panggil dlopen(scratch, RTLD_NOW) di thread utama game.
//
// Exit code (dipetakan InjectorBridge.failureReason):
//   0 OK · 2 argumen salah · 3 libdl/libc tidak ketemu di proses ini
//   4 ptrace attach ditolak · 5 libc/libdl tidak ketemu di proses game
//   6 alokasi mmap gagal · 7 tulis path gagal
//   8 panggilan dlopen tidak selesai (timeout)
//   9 dlopen NULL — alasan linker sebenarnya dibaca lewat dlerror()
//     remote dan ditulis ke hozinsniper_inject.log + stderr.
#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <vector>

#include "proc_maps.h"
#include "remote_call_arm64.h"

#define LOG_TAG "HozinSniper3D"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

FILE* g_log = nullptr;

void log_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", buf);
    if (g_log) {
        std::fprintf(g_log, "%s\n", buf);
        std::fflush(g_log);
    }
}

void log_err(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "%s", buf);
    if (g_log) {
        std::fprintf(g_log, "%s\n", buf);
        std::fflush(g_log);
    }
}

// Offset fn di dalam lib yang memetakannya di proses INI. Resolusi
// alamat remote (base terendah mapping + offset ini) hanya bekerja
// karena file library di disk identik di kedua proses — ASLR hanya
// menggeser base, bukan offset internal.
bool local_offset(const char* lib_name, void* fn, uintptr_t& offset_out) {
    injector::ModuleInfo mod{};
    if (!injector::find_module(0, lib_name, mod)) {
        log_err("could not find local module %s", lib_name);
        return false;
    }
    offset_out = reinterpret_cast<uintptr_t>(fn) - mod.base;
    return true;
}

bool resolve_remote(int pid, const char* lib_name, uintptr_t offset, uintptr_t& addr_out) {
    injector::ModuleInfo mod{};
    if (!injector::find_module(pid, lib_name, mod)) {
        log_err("could not find remote module %s in pid %d", lib_name, pid);
        return false;
    }
    addr_out = mod.base + offset;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    g_log = std::fopen("/data/local/tmp/hozinsniper_inject.log", "w");
    if (argc != 3) {
        std::fprintf(stderr, "usage: injector <pid> <payload.so path>\n");
        if (g_log) std::fclose(g_log);
        return 2;
    }

    int pid = std::atoi(argv[1]);
    const char* payload_path = argv[2];
    if (pid <= 0 || std::strlen(payload_path) >= 0x1000) {
        log_err("bad pid or payload path too long");
        if (g_log) std::fclose(g_log);
        return 2;
    }

    uintptr_t dlopen_offset = 0, mmap_offset = 0, dlerror_offset = 0;
    if (!local_offset("libdl.so", reinterpret_cast<void*>(&dlopen), dlopen_offset) ||
        !local_offset("libc.so", reinterpret_cast<void*>(&mmap), mmap_offset) ||
        !local_offset("libdl.so", reinterpret_cast<void*>(&dlerror), dlerror_offset)) {
        if (g_log) std::fclose(g_log);
        return 3;
    }

    injector::RemoteProcess proc;
    if (!proc.attach(pid)) {
        log_err("attach to pid %d failed -- confirm the shell running this is actually root "
                "(id -u == 0), and check getenforce if it still fails", pid);
        if (g_log) std::fclose(g_log);
        return 4;
    }

    uintptr_t remote_mmap = 0, remote_dlopen = 0, remote_dlerror = 0;
    if (!resolve_remote(pid, "libc.so", mmap_offset, remote_mmap) ||
        !resolve_remote(pid, "libdl.so", dlopen_offset, remote_dlopen) ||
        !resolve_remote(pid, "libdl.so", dlerror_offset, remote_dlerror)) {
        log_err("could not resolve remote dlopen/mmap/dlerror, bionic layout may differ "
                "on this Android build; check /proc/%d/maps manually", pid);
        proc.detach();
        if (g_log) std::fclose(g_log);
        return 5;
    }
    log_line("remote mmap=0x%lx remote dlopen=0x%lx", remote_mmap, remote_dlopen);

    // 1) allocate RW scratch in the target for the payload path string.
    uint64_t scratch = 0;
    std::vector<uint64_t> mmap_args = {
        0,                                     // addr
        0x1000,                                // length
        PROT_READ | PROT_WRITE,                // prot
        MAP_ANONYMOUS | MAP_PRIVATE,            // flags
        static_cast<uint64_t>(-1),              // fd
        0                                        // offset
    };
    if (!proc.call(remote_mmap, mmap_args, scratch) || scratch == 0 ||
        scratch == static_cast<uint64_t>(-1)) {
        log_err("remote mmap failed");
        proc.detach();
        if (g_log) std::fclose(g_log);
        return 6;
    }

    // 2) write the payload path into that scratch region.
    size_t path_len = std::strlen(payload_path) + 1;
    if (!proc.write_mem(scratch, payload_path, path_len)) {
        log_err("failed writing payload path into remote scratch at 0x%lx", scratch);
        proc.detach();
        if (g_log) std::fclose(g_log);
        return 7;
    }

    // 3) call dlopen(scratch, RTLD_NOW) in the target.
    uint64_t handle = 0;
    std::vector<uint64_t> dlopen_args = {scratch, RTLD_NOW};
    bool called = proc.call(remote_dlopen, dlopen_args, handle);

    if (!called) {
        log_err("remote dlopen call did not complete");
        proc.detach();
        if (g_log) std::fclose(g_log);
        return 8;
    }
    if (handle == 0) {
        // Baca alasan linker yang sebenarnya lewat dlerror() remote.
        uint64_t err_ptr = 0;
        std::vector<uint64_t> no_args;
        if (proc.call(remote_dlerror, no_args, err_ptr) && err_ptr != 0) {
            char err_buf[1024] = {0};
            if (proc.read_mem(err_ptr, err_buf, sizeof(err_buf) - 1)) {
                log_err("remote dlopen returned NULL. dlerror: %s", err_buf);
            }
        }
        log_err("remote dlopen returned NULL. On Android 8+ this is often a linker "
                "namespace rejection rather than a missing file.");
        proc.detach();
        if (g_log) std::fclose(g_log);
        return 9;
    }

    proc.detach();
    log_line("dlopen succeeded in pid %d, handle=0x%lx", pid, handle);
    std::printf("OK handle=0x%lx\n", handle);
    if (g_log) std::fclose(g_log);
    return 0;
}
